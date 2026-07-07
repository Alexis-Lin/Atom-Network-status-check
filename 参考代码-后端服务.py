# =============================================================================
#  ATOM 网络状态「红绿灯」— 后端服务参考实现（FastAPI）
# -----------------------------------------------------------------------------
#  对应 PRD：PRD-atom网络状态检测与信号同步.md
#    B5 上报          → POST /v1/net/report        （批量样本 + 测速结果入库）
#    B2/B4 阈值 OTA   → GET  /v1/net/config        （net_config_t 全字段下发）
#    B3 备选文件测速  → GET  /v1/speedtest/ping
#                       GET  /v1/speedtest/download（定长随机字节流）
#                       POST /v1/speedtest/upload  （收流计时后丢弃）
#    客服排查         → GET  /v1/net/timeline      （按设备聚合的时间线）
#
#  性质说明：可运行的参考骨架（uvicorn 后端参考代码-后端服务:app）。存储用
#  SQLite 便于演示，生产替换为时序库/数仓；鉴权、限流、多租户从公司基建接入。
#
#  ⚠ 文件测速端点的三条纪律（决定测出来的是不是真实链路）：
#    1. 必须直连业务同机房/同链路，禁止套 CDN——CDN 测的不是上课链路；
#    2. 响应禁用 gzip/缓存（Cache-Control: no-store），字节即字节；
#    3. 服务端按设备限并发（同一设备同时仅 1 个测速会话）+ 全局限流，
#       防止测速流量冲击业务带宽（对齐 PC 教练端实现，接口人：张恒）。
# =============================================================================

import json
import os
import sqlite3
import time

from fastapi import FastAPI, Request, Response
from fastapi.responses import StreamingResponse

app = FastAPI(title="atom-net-status", version="1.1")
DB = os.environ.get("NET_DB", "net_status.db")

# ---- 存储 -------------------------------------------------------------------

def db():
    conn = sqlite3.connect(DB)
    conn.execute("""CREATE TABLE IF NOT EXISTS probe_samples(
        device_id TEXT, ts INTEGER, ssid_hash TEXT, rssi INTEGER,
        rtt_gw INTEGER, rtt_srv INTEGER, loss INTEGER, light INTEGER)""")
    conn.execute("""CREATE TABLE IF NOT EXISTS speedtests(
        device_id TEXT, ts INTEGER, ssid_hash TEXT, success INTEGER,
        quality INTEGER, rtt INTEGER, up_loss INTEGER, down_loss INTEGER,
        up_kbps INTEGER, down_kbps INTEGER, duration_s INTEGER,
        curve_json TEXT, err TEXT)""")
    conn.execute("""CREATE TABLE IF NOT EXISTS class_quality(
        device_id TEXT, ts INTEGER, class_id TEXT, light INTEGER)""")
    return conn

# ---- B2/B4 · 阈值配置 OTA 下发 ----------------------------------------------
# 与设备端 net_config_t 一一对应；device 侧比对 version 决定是否应用。

NET_CONFIG = {
    "version": 3,
    "probe_interval_active_sec": 30,
    "probe_interval_idle_min": 60,
    "freshness_window_sec": 300,
    "rssi_green_dbm": -60, "rssi_yellow_dbm": -70,
    "rtt_green_ms": 50, "rtt_yellow_ms": 150,
    "loss_green_pct": 2, "loss_yellow_pct": 8,
    "debounce_samples": 3,
    "weak_streak_notify": 3,
    "expected_up_kbps": 1500, "expected_down_kbps": 2000,
    "speedtest_timeout_sec": 40,
}

@app.get("/v1/net/config")
def get_config(device_id: str, version: int = 0):
    # 灰度：可按 device_id 分桶下发不同阈值做 A/B 校准（PRD B4）
    if version >= NET_CONFIG["version"]:
        return Response(status_code=304)
    return NET_CONFIG

# ---- B5 · 批量上报 -----------------------------------------------------------
# 设备侧离线环形缓存 + 网络恢复补传；单条坏数据跳过不阻塞整批。
# 时钟未同步的设备用相对时间戳（ts_rel），落库时以服务端时间回推。

@app.post("/v1/net/report")
async def report(req: Request):
    body = await req.json()
    now = int(time.time())
    dev = body["device_id"]
    base = now - body.get("uptime_sec", 0)          # 相对时间戳回推
    conn = db()
    for s in body.get("probe_samples", []):
        try:
            conn.execute("INSERT INTO probe_samples VALUES(?,?,?,?,?,?,?,?)",
                (dev, base + s["ts_rel"], s["ssid_hash"], s["rssi"],
                 s["rtt_gw"], s["rtt_srv"], s["loss"], s["light"]))
        except Exception:
            continue                                 # 坏样本跳过
    for t in body.get("speedtests", []):
        try:
            conn.execute("INSERT INTO speedtests VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (dev, base + t["ts_rel"], t["ssid_hash"], t["success"],
                 t.get("quality"), t.get("rtt"), t.get("up_loss"), t.get("down_loss"),
                 t.get("up_kbps"), t.get("down_kbps"), t.get("duration_s"),
                 json.dumps(t.get("curve", [])), t.get("err", "")))
        except Exception:
            continue
    for c in body.get("class_quality", []):
        conn.execute("INSERT INTO class_quality VALUES(?,?,?,?)",
                     (dev, base + c["ts_rel"], c["class_id"], c["light"]))
    conn.commit(); conn.close()
    return {"ok": True}

# ---- 客服排查 · 设备时间线（PRD B5 / M3）------------------------------------
# 返回近 N 天的样本/测速/课中灯色聚合；「连续 3 天课中红灯」的长期建议
# 触发条件也由这份数据算出、随 config 下发标志位（或由设备本地统计）。

@app.get("/v1/net/timeline")
def timeline(device_id: str, days: int = 7):
    since = int(time.time()) - days * 86400
    conn = db()
    q = lambda sql: [dict(zip([c[0] for c in cur.description], r))
                     for cur in [conn.execute(sql, (device_id, since))]
                     for r in cur.fetchall()]
    out = {
        "probes":     q("SELECT ts,ssid_hash,light,rtt_srv,loss FROM probe_samples WHERE device_id=? AND ts>? ORDER BY ts"),
        "speedtests": q("SELECT ts,ssid_hash,success,quality,rtt,up_kbps,down_kbps,duration_s,curve_json FROM speedtests WHERE device_id=? AND ts>? ORDER BY ts"),
        "class":      q("SELECT ts,class_id,light FROM class_quality WHERE device_id=? AND ts>? ORDER BY ts"),
    }
    # 连续课中红灯天数（喂给设备端 on_longterm_red_days）
    red_days, day = 0, None
    for c in reversed(out["class"]):
        d = c["ts"] // 86400
        if c["light"] == 3 and d != day:
            red_days += 1; day = d
        elif c["light"] != 3:
            break
    out["consecutive_red_days"] = red_days
    conn.close()
    return out

# ---- B3 备选 · 文件测速（TRTC 课外不可用时的自研路径）------------------------
# 固定时间窗设计：下行 8s + 上行 8s + ping ≈ 2s，总时长恒定约 18s（PRD A4.7）。
# 设备端按 1s 吞吐量桶采样（15–30 个波形点），阈值折算复用 judge_resident。

CHUNK = 64 * 1024
NO_STORE = {"Cache-Control": "no-store", "Content-Encoding": "identity"}

@app.get("/v1/speedtest/ping")
def st_ping():
    return Response(status_code=204, headers=NO_STORE)   # RTT/丢包：设备侧计时

@app.get("/v1/speedtest/download")
def st_download(bytes: int = 4 * 1024 * 1024):
    bytes = min(bytes, 16 * 1024 * 1024)                  # 上限保护
    def gen(n=bytes):
        blk = os.urandom(CHUNK)                           # 随机字节防中间设备压缩
        while n > 0:
            yield blk[:min(CHUNK, n)]; n -= CHUNK
    return StreamingResponse(gen(), media_type="application/octet-stream",
                             headers={**NO_STORE, "Content-Length": str(bytes)})

@app.post("/v1/speedtest/upload")
async def st_upload(req: Request):
    t0, n = time.monotonic(), 0
    async for chunk in req.stream():                      # 收流计时，内容丢弃
        n += len(chunk)
    ms = int((time.monotonic() - t0) * 1000)
    return {"bytes": n, "ms": ms, "kbps": int(n * 8 / max(ms, 1))}
