/*
 * ============================================================================
 *  ATOM 网络状态「红绿灯」— 检测调度 / 状态机 / TRTC 接线 · C 参考实现
 * ----------------------------------------------------------------------------
 *  对应 PRD：PRD-atom网络状态检测与信号同步.md
 *    - 模块 B（检测逻辑）：B1 数据源 / B2 采样调度与时效 / B3 测速实现 / B4 阈值防抖
 *    - 模块 A（界面显示）：通过 ui_notify_* 回调驱动，各入口共享同一状态机
 *    - 模块 C（课中）：on_trtc_network_quality() 一节
 *
 *  性质说明：本文件是「可直接抄的骨架 + 伪代码」——数据结构、状态机、阈值、
 *  防抖、调度全部可直接使用；标注 [PLATFORM] 的函数依赖平台能力（定时器、
 *  ping、TRTC SDK、UI），需接到实际工程的对应实现上。
 * ============================================================================
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

/* ========================================================================== */
/*  1. 基础类型（对应 PRD §3.1 三态 + 检测中/失败）                              */
/* ========================================================================== */

typedef enum {
    LIGHT_TESTING = 0,   /* 三格全暗：无有效数据 / 检测中（不外显旧灯色） */
    LIGHT_GREEN   = 1,   /* 网速良好：3 格绿  —— 静默，不打扰            */
    LIGHT_YELLOW  = 2,   /* 网速一般：2 格黄  —— 轻提示                  */
    LIGHT_RED     = 3,   /* 网速较慢：1 格红  —— 提示 + 建议             */
} light_t;

typedef enum {                    /* 归因分段（PRD B4 归因探测） */
    SEG_OK = 0,
    SEG_DEVICE_TO_ROUTER,         /* 网关 ping 差：设备→路由器弱，建议挪位置   */
    SEG_ROUTER_TO_INTERNET,       /* 网关好、服务器差：宽带/公网弱，建议换网   */
} weak_segment_t;

/* 一次常驻轻探测的样本（PRD B1「轻探测」：ping 网关 + 自家服务器，~0.2KB/轮） */
typedef struct {
    uint32_t ts_sec;              /* 采样时间戳（单调秒）                     */
    int16_t  rssi_dbm;            /* 本地信号强度（仅详情页展示，不进灯色）？  */
                                  /* 注意：RSSI 参与灯色判定，见 judge_resident */
    uint16_t rtt_gw_ms;           /* 到网关 RTT                               */
    uint16_t rtt_srv_ms;          /* 到自家服务器 RTT                         */
    uint8_t  loss_pct;            /* 丢包率 0–100                             */
    bool     valid;
} probe_sample_t;

/* 完整测速结果（PRD B3：字段与 TRTCSpeedTestResult 一一对应）                */
typedef struct {
    bool     success;             /* false → 失败态：不给灯色（A4.5）          */
    uint8_t  quality;             /* TRTCQuality 1–6；灯色直接采用官方评分     */
    uint16_t rtt_ms;
    uint8_t  up_loss_pct;         /* upLostRate * 100                          */
    uint8_t  down_loss_pct;
    int32_t  up_kbps;             /* availableUpBandwidth，-1 = 无效           */
    int32_t  down_kbps;
    uint8_t  sample_count;        /* 波形样本数（节点数，约 5–8）              */
    uint16_t up_curve[8];         /* 波形数据：逐节点上行/下行 kbps 采样        */
    uint16_t down_curve[8];
    char     err_msg[64];         /* 仅上报，不上屏                            */
} speedtest_result_t;

/* ========================================================================== */
/*  2. 可 OTA 下发的配置（PRD B2 / B4：首发保守值，全部可远程调整）              */
/* ========================================================================== */

typedef struct {
    /* 采样周期 */
    uint16_t probe_interval_active_sec;   /* 亮屏交互态轻探测周期，默认 30    */
    uint16_t probe_interval_idle_min;     /* 桌面常亮态低频采样，默认 60      */
    uint16_t freshness_window_sec;        /* 数据新鲜窗口，默认 300（5 分钟） */
    /* 阈值（常驻轻探测 → 灯色，PRD B4 表） */
    int16_t  rssi_green_dbm;              /* > -60 → 绿                        */
    int16_t  rssi_yellow_dbm;             /* -60 ~ -70 → 黄，更低 → 红          */
    uint16_t rtt_green_ms;                /* < 50 → 绿                         */
    uint16_t rtt_yellow_ms;               /* 50–150 → 黄，更高 → 红             */
    uint8_t  loss_green_pct;              /* < 2 → 绿                          */
    uint8_t  loss_yellow_pct;             /* 2–8 → 黄，更高 → 红                */
    /* 防抖与提示 */
    uint8_t  debounce_samples;            /* 连续 N 个同级样本才切灯，默认 3   */
    uint8_t  weak_streak_notify;          /* 同 WiFi 连续 N 次黄/红 → 通知，默认 3 */
    /* 测速期望带宽（PRD B3） */
    uint16_t expected_up_kbps;            /* 默认 1500                         */
    uint16_t expected_down_kbps;          /* 默认 2000                         */
    /* 测速时长非固定（节点数×网络状况，典型 10–30s），只设硬超时（PRD A4.7） */
    uint8_t  speedtest_timeout_sec;       /* 默认 40，超时→失败态「检测超时」   */
} net_config_t;

static net_config_t g_cfg = {
    .probe_interval_active_sec = 30,
    .probe_interval_idle_min   = 60,
    .freshness_window_sec      = 300,
    .rssi_green_dbm  = -60, .rssi_yellow_dbm = -70,
    .rtt_green_ms    = 50,  .rtt_yellow_ms   = 150,
    .loss_green_pct  = 2,   .loss_yellow_pct = 8,
    .debounce_samples   = 3,
    .weak_streak_notify = 3,
    .expected_up_kbps   = 1500,
    .expected_down_kbps = 2000,
    .speedtest_timeout_sec = 40,
};

/* ========================================================================== */
/*  3. 全局状态（按 SSID 隔离缓存——灯色不跨 WiFi 继承，PRD B2）                 */
/* ========================================================================== */

typedef struct {
    char           ssid[33];
    light_t        shown_light;          /* 当前对外展示的灯色               */
    light_t        pending_light;        /* 防抖中的候选灯色                 */
    uint8_t        pending_count;        /* 候选灯色已连续出现的次数         */
    uint8_t        weak_streak;          /* 连续黄/红计数（A3.3 短期通知）    */
    bool           weak_notified_today;  /* 每 WiFi 每天最多提示一次         */
    probe_sample_t last_sample;
} net_state_t;

static net_state_t g_net;                /* 当前连接网络的状态               */
static bool        g_in_class   = false; /* 课中标志：测速互斥 + 通知抑制     */
static bool        g_testing    = false; /* 完整测速进行中（防重入）          */

/* ---- 平台依赖 [PLATFORM]，接到实际工程 ---------------------------------- */
extern uint32_t plat_uptime_sec(void);
extern bool     plat_ping(const char *host, uint16_t *rtt_ms, uint8_t *loss_pct);
extern int16_t  plat_wifi_rssi(void);
extern void     plat_timer_start(int id, uint32_t period_ms, void (*cb)(void));
extern void     plat_timer_stop(int id);
/* UI 通知：模块 A 各入口（WiFi 状态行 / 磁贴 / 通知栏 / 小程序）统一订阅       */
extern void     ui_notify_light(light_t light, uint32_t sample_age_sec);
extern void     ui_notify_speedtest_progress(uint8_t finished, uint8_t total,
                                             uint16_t up_kbps, uint16_t down_kbps);
extern void     ui_notify_speedtest_result(const speedtest_result_t *r,
                                           weak_segment_t seg, bool unstable);
extern void     ui_push_notification(const char *text);   /* 通知栏推送        */
extern void     report_upload(const char *json);          /* B5 云端上报       */

/* ========================================================================== */
/*  4. 常驻轻探测：采样 → 判级 → 防抖 → 外显（PRD B1/B2/B4）                    */
/* ========================================================================== */

/* 单个样本 → 灯色（多指标取较差者） */
static light_t judge_resident(const probe_sample_t *s)
{
    light_t by_rssi = (s->rssi_dbm > g_cfg.rssi_green_dbm)  ? LIGHT_GREEN
                    : (s->rssi_dbm > g_cfg.rssi_yellow_dbm) ? LIGHT_YELLOW : LIGHT_RED;
    uint16_t rtt   = (s->rtt_srv_ms > s->rtt_gw_ms) ? s->rtt_srv_ms : s->rtt_gw_ms;
    light_t by_rtt  = (rtt < g_cfg.rtt_green_ms)  ? LIGHT_GREEN
                    : (rtt < g_cfg.rtt_yellow_ms) ? LIGHT_YELLOW : LIGHT_RED;
    light_t by_loss = (s->loss_pct < g_cfg.loss_green_pct)  ? LIGHT_GREEN
                    : (s->loss_pct < g_cfg.loss_yellow_pct) ? LIGHT_YELLOW : LIGHT_RED;
    light_t worst = by_rssi;
    if (by_rtt  > worst) worst = by_rtt;
    if (by_loss > worst) worst = by_loss;
    return worst;
}

/* 归因：网关差 → 设备→路由器段；网关好服务器差 → 宽带/公网段（PRD B4） */
static weak_segment_t attribute_segment(const probe_sample_t *s)
{
    if (s->rtt_gw_ms  >= g_cfg.rtt_yellow_ms) return SEG_DEVICE_TO_ROUTER;
    if (s->rtt_srv_ms >= g_cfg.rtt_yellow_ms) return SEG_ROUTER_TO_INTERNET;
    return SEG_OK;
}

/* 防抖：连续 debounce_samples 个同级样本才切灯（避免角标闪烁） */
static void apply_debounced(light_t candidate)
{
    if (candidate == g_net.shown_light) { g_net.pending_count = 0; return; }
    if (candidate == g_net.pending_light) {
        if (++g_net.pending_count >= g_cfg.debounce_samples) {
            g_net.shown_light   = candidate;
            g_net.pending_count = 0;
            ui_notify_light(candidate, 0);
        }
    } else {
        g_net.pending_light = candidate;
        g_net.pending_count = 1;
    }
}

/* ==========================================================================
 *  4b. 通知条目：唯一网络位 + 内容升级状态机（PRD A3，取代独立推送卡片）
 *  通知栏网络信息只有一个条目：事件不新增卡片，只升级条目内容。
 *  优先级：长期建议(2) > 短期弱网(1) > 实时状态(0)；绿灯永不触发升级。
 * ========================================================================== */

typedef enum {
    NSLOT_REALTIME    = 0,   /* 默认：绿良好 / 黄一般·点击测速 / 红较慢·查看建议 */
    NSLOT_SHORT_WEAK  = 1,   /* 「当前 WiFi 网速较弱 / 可能影响上课」            */
    NSLOT_LONG_ADVICE = 2,   /* 「上课网络较弱 / 建议靠近路由器」                */
} nslot_level_t;

#define NSLOT_LONG_FALLBACK_SEC (7u * 24 * 3600)  /* 长期建议未点击 7 天自动回落 */

static nslot_level_t g_nslot    = NSLOT_REALTIME;
static uint32_t      g_nslot_ts = 0;

extern void ui_notify_slot_render(int level, light_t realtime); /* [PLATFORM] 重绘条目 */
extern void ui_open_speedtest_app(void);                        /* [PLATFORM]          */

static void nslot_escalate(nslot_level_t lv)
{
    if (g_in_class || lv <= g_nslot) return;   /* 课中不打扰；只升不降           */
    g_nslot = lv; g_nslot_ts = plat_uptime_sec();
    ui_notify_slot_render(g_nslot, g_net.shown_light);
}

void on_notify_slot_clicked(void)              /* 任何内容态点击都进小程序（A3.1）*/
{
    g_nslot = NSLOT_REALTIME;                  /* 点击即回落                     */
    ui_open_speedtest_app();
}

void nslot_minutely_tick(void)                 /* 挂到分钟级定时器               */
{
    if (g_nslot == NSLOT_LONG_ADVICE &&
        plat_uptime_sec() - g_nslot_ts > NSLOT_LONG_FALLBACK_SEC)
        g_nslot = NSLOT_REALTIME;              /* 超时回落                       */
    if (g_nslot == NSLOT_SHORT_WEAK && g_net.shown_light == LIGHT_GREEN)
        g_nslot = NSLOT_REALTIME;              /* 恢复绿灯回落                   */
    ui_notify_slot_render(g_nslot, g_net.shown_light);
}

/* 连续 N 天课中红灯（由 B5 统计喂入）→ 升级长期建议，7 天冷却不重复 */
void on_longterm_red_days(uint8_t consecutive_days)
{
    static uint32_t last_escalate_sec = 0;
    if (consecutive_days >= 3 &&
        plat_uptime_sec() - last_escalate_sec > NSLOT_LONG_FALLBACK_SEC) {
        nslot_escalate(NSLOT_LONG_ADVICE);
        last_escalate_sec = plat_uptime_sec();
    }
}

/* 短期弱网：同 WiFi 连续 3 次黄/红 → 条目升级（每 WiFi 每天最多一次，PRD A3.3） */
static void maybe_notify_weak_streak(light_t l)
{
    if (l >= LIGHT_YELLOW) {
        if (++g_net.weak_streak >= g_cfg.weak_streak_notify
            && !g_net.weak_notified_today && !g_in_class) {
            nslot_escalate(NSLOT_SHORT_WEAK);
            g_net.weak_notified_today = true;
        }
    } else {
        g_net.weak_streak = 0;
    }
}

/* ==========================================================================
 *  4c. 指标独立着色（PRD A4.3 着色标准表；阈值与 B4 同源、OTA 可调）
 *  返回 LIGHT_GREEN 时 UI 渲染为默认白色；整页灯色仍以 TRTC quality 为准。
 * ========================================================================== */

light_t metric_color_rtt(uint16_t ms)
{
    return ms <= g_cfg.rtt_green_ms  ? LIGHT_GREEN
         : ms <= g_cfg.rtt_yellow_ms ? LIGHT_YELLOW : LIGHT_RED;
}

light_t metric_color_loss(uint8_t pct)
{
    return pct <  g_cfg.loss_green_pct  ? LIGHT_GREEN
         : pct <= g_cfg.loss_yellow_pct ? LIGHT_YELLOW : LIGHT_RED;
}

/* kbps = 0xFFFF 表示官方 -1 无效值：UI 显示「—」、不着色、不入波形 */
light_t metric_color_bw(uint16_t kbps, uint16_t expected_kbps)
{
    if (kbps == 0xFFFF) return LIGHT_TESTING;
    if (kbps >= expected_kbps) return LIGHT_GREEN;
    return (uint32_t)kbps * 10 >= (uint32_t)expected_kbps * 6   /* ≥60% 期望 */
         ? LIGHT_YELLOW : LIGHT_RED;
}

/* 执行一次轻探测（约 0.2 KB；调用点见第 5 节调度） */
static void run_light_probe(void)
{
    probe_sample_t s = { .ts_sec = plat_uptime_sec() };
    uint16_t rtt; uint8_t loss;
    s.rssi_dbm = plat_wifi_rssi();
    if (plat_ping("192.168.1.1"   /* 网关，实际从 DHCP 取 */, &rtt, &loss)) {
        s.rtt_gw_ms = rtt; s.loss_pct = loss;
    } else { s.rtt_gw_ms = 999; s.loss_pct = 100; }
    if (plat_ping("net-probe.fiture.com" /* 自家探测端点 */, &rtt, &loss)) {
        s.rtt_srv_ms = rtt;
        if (loss > s.loss_pct) s.loss_pct = loss;
    } else { s.rtt_srv_ms = 999; }
    s.valid = true;
    g_net.last_sample = s;

    light_t l = judge_resident(&s);
    apply_debounced(l);
    maybe_notify_weak_streak(l);
}

/* 时效检查：UI 取灯色前必须调用（PRD B2：>5 分钟不外显旧灯色） */
light_t net_current_light(uint32_t *age_sec_out)
{
    uint32_t age = plat_uptime_sec() - g_net.last_sample.ts_sec;
    if (age_sec_out) *age_sec_out = age;
    if (!g_net.last_sample.valid || age > g_cfg.freshness_window_sec)
        return LIGHT_TESTING;        /* 过期 → 三格全暗「检测中…」并触发刷新 */
    return g_net.shown_light;
}

/* ========================================================================== */
/*  5. 采样调度：事件 → 采样（PRD B2 调度表逐行对应）                           */
/* ========================================================================== */

#define TIMER_ACTIVE_PROBE  1
#define TIMER_IDLE_PROBE    2

void on_wifi_provisioned(void)   { run_light_probe(); }   /* ① 首次配网成功    */
void on_boot_or_wake(void)       { run_light_probe(); }   /* ② 开机/休眠唤醒   */

void on_screen_interactive(void) {                        /* ③ 亮屏交互态      */
    plat_timer_stop(TIMER_IDLE_PROBE);
    plat_timer_start(TIMER_ACTIVE_PROBE,
                     g_cfg.probe_interval_active_sec * 1000u, run_light_probe);
}
void on_screen_idle_clock(void) {                         /* ④ 桌面常亮态      */
    plat_timer_stop(TIMER_ACTIVE_PROBE);
    plat_timer_start(TIMER_IDLE_PROBE,
                     g_cfg.probe_interval_idle_min * 60u * 1000u, run_light_probe);
}
void on_screen_off_or_sleep(void) {                       /* ⑦ 熄屏/深睡：不采样 */
    plat_timer_stop(TIMER_ACTIVE_PROBE);
    plat_timer_stop(TIMER_IDLE_PROBE);
}

void on_wifi_changed(const char *new_ssid) {              /* ⑤ 切网：清缓存立测 */
    memset(&g_net, 0, sizeof(g_net));                     /*   灯色不跨 WiFi 继承 */
    strncpy(g_net.ssid, new_ssid, sizeof(g_net.ssid) - 1);
    ui_notify_light(LIGHT_TESTING, 0);                    /*   先「检测中…」     */
    run_light_probe();
}

void on_class_enter(void) {                               /* ⑥ 进课：非阻塞可选 */
    g_in_class = true;
    if (g_testing) speedtest_cancel();                    /*   课中互斥          */
    run_light_probe();                                    /*   fire-and-forget，*/
                                                          /*   不等待、不阻塞    */
}
void on_class_exit(void) { g_in_class = false; }

/* ========================================================================== */
/*  6. 完整测速：TRTC 主方案 + 文件测速备选，共用同一状态机（PRD B3）            */
/* ========================================================================== */

/* [PLATFORM] 二选一实现（UI/状态机/文案完全一致）：
 *   方案一（主）：TRTC startSpeedTest
 *     TRTCSpeedTestParams p = { sdkAppId, userId, userSig,
 *                               .expectedUpBandwidth  = g_cfg.expected_up_kbps,
 *                               .expectedDownBandwidth= g_cfg.expected_down_kbps,
 *                               .scene = DelayAndBandwidthTesting };
 *     trtc->startSpeedTest(p);   → onSpeedTestResult 逐节点回调（每 2–3 秒）
 *   方案二（备选，参考 PC 教练端 / 张恒）：
 *     对自家服务器 上传+下载 测试文件，数秒内多次采样，
 *     quality 由 judge_resident 的阈值表折算。                              */
extern bool plat_speedtest_start(void);
extern void plat_speedtest_stop(void);

extern void plat_timer_once(const char *name, uint32_t sec, void (*fn)(void));
extern void plat_timer_cancel(const char *name);
extern void ui_show_speedtest_timeout(void);   /* 失败态变体「检测超时」        */

static void on_speedtest_timeout(void)
{
    /* 时长由节点数与网络状况决定（快网不会明显更快、差网会拖长），
     * 不设固定时长，只兜硬超时；已回传样本仍随上报带出 */
    plat_speedtest_stop();
    g_testing = false;
    ui_show_speedtest_timeout();
}

bool speedtest_start(void)
{
    if (g_in_class) return false;        /* 课中互斥（按钮也应置灰）          */
    if (g_testing)  return false;        /* 官方限制：同一时间仅一个任务       */
    g_testing = true;
    plat_timer_once("st_timeout", g_cfg.speedtest_timeout_sec, on_speedtest_timeout);
    return plat_speedtest_start();
}

void speedtest_cancel(void)
{
    if (!g_testing) return;
    plat_timer_cancel("st_timeout");
    plat_speedtest_stop();               /* TRTC: stopSpeedTest()             */
    g_testing = false;
}

/* 逐节点进度回调 → 驱动环形进度 + 波形逐点生长（A4.2，不做假进度） */
void on_speedtest_progress(uint8_t finished, uint8_t total,
                           uint16_t up_kbps, uint16_t down_kbps)
{
    ui_notify_speedtest_progress(finished, total, up_kbps, down_kbps);
}

/* 稳定性派生标签：样本方差超阈值 →「网络波动较大」（A4.3） */
static bool curve_unstable(const uint16_t *curve, uint8_t n)
{
    if (n < 3) return false;
    uint32_t sum = 0, sq = 0;
    for (uint8_t i = 0; i < n; i++) { sum += curve[i]; sq += (uint32_t)curve[i] * curve[i]; }
    uint32_t mean = sum / n;
    uint32_t var  = sq / n - mean * mean;
    return mean > 0 && var > (mean * mean / 4);   /* 变异系数 > ~50% 视为波动大 */
}

/* 最终结果回调 → 结果页三色 / 失败态（A4.3–A4.5） */
void on_speedtest_result(const speedtest_result_t *r)
{
    plat_timer_cancel("st_timeout");
    g_testing = false;

    if (!r->success) {                   /* 失败态：不给灯色——没测出来≠网络差 */
        ui_notify_speedtest_result(r, SEG_OK, false);
        /* err_msg 仅随上报，不上屏 */
        report_upload("{\"type\":\"speedtest_fail\", ...}");
        return;
    }

    /* 灯色直接采用官方 quality：1–2 绿 / 3–4 黄 / 5–6 红（与课中同一映射） */
    light_t l = (r->quality <= 2) ? LIGHT_GREEN
              : (r->quality <= 4) ? LIGHT_YELLOW : LIGHT_RED;
    g_net.shown_light = l;               /* 测速结果直接刷新常驻灯色           */
    g_net.last_sample.ts_sec = plat_uptime_sec();
    ui_notify_light(l, 0);

    bool unstable = curve_unstable(r->up_curve,   r->sample_count)
                 || curve_unstable(r->down_curve, r->sample_count);
    ui_notify_speedtest_result(r, attribute_segment(&g_net.last_sample), unstable);
    report_upload("{\"type\":\"speedtest\", ...}");   /* 逐次上报（B5）        */
}

/* ========================================================================== */
/*  7. 课中：onNetworkQuality → bars 角标 + toast 冷却（模块 C1，供二期）        */
/* ========================================================================== */

static uint32_t g_last_toast_sec  = 0;
static uint8_t  g_toast_count     = 0;   /* 每节课最多 2 次，冷却 5 分钟       */

void on_trtc_network_quality(uint8_t local_q, uint8_t remote_q)
{
    uint8_t worst = (local_q > remote_q) ? local_q : remote_q;
    light_t l = (worst <= 2) ? LIGHT_GREEN
              : (worst <= 4) ? LIGHT_YELLOW : LIGHT_RED;

    apply_debounced(l);                  /* 课中同样防抖：约 6 秒（3×2s 回调） */

    if (l >= LIGHT_YELLOW && g_toast_count < 2
        && plat_uptime_sec() - g_last_toast_sec > 300) {
        /* 方向归因：local 差 =「你的画面可能卡顿」，remote 差 =「课程画面可能卡顿」 */
        const char *msg = (l == LIGHT_YELLOW) ? "网络一般 · 已降低画质"
                        : (local_q >= remote_q) ? "网络较差 · 你的画面可能卡顿"
                                                : "网络较差 · 课程画面可能卡顿";
        ui_push_notification(msg);       /* 课中为 toast + 短音效（C1）        */
        g_last_toast_sec = plat_uptime_sec();
        g_toast_count++;
    }
    /* 恢复不提示，静默变绿（apply_debounced 已处理） */
}

/* ============================== 文件结束 =================================== */
