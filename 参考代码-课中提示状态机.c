/*
 * ============================================================================
 *  ATOM 课中提示 · 三级响应状态机 — weak_network_manager.c 的改造参考实现
 * ----------------------------------------------------------------------------
 *  对应 PRD：PRD-3-课中双端提示.md（§2 三级响应 / §2.2 函数级映射 / §2.4 事件表）
 *  改造自：backup/现状代码-weak_network_manager.c（保留其防抖、幂等、
 *          pending_ctx 请求重放、pre_page、线程边界等机制，符号名尽量对齐，
 *          方便对照 diff）
 *
 *  设计总纲（不打扰原则的代码化）：
 *    「打扰」被分成三级，级别只由事件严重度决定，用户永远不需要为
 *    「网络慢」做任何操作——重试是机器的活。
 *
 *    抽屉（轻度）  —— 穿透式底部面板（顶角 r40、底边出屏）：灯色「下降沿」
 *                     弹一次，升起 0.45s → 停留 3s → 收回 0.45s，零按钮；
 *                     冷却 300s + 每课 ≤2 次双重限流。课中无独立常驻角标，
 *                     bars 永远随容器出现（评审决议）。
 *    重试卡（中度）—— 居中双行卡：事件行 + 蓝色进度行（第 N 次外显），
 *                     断线自动重连中 / 关键请求重试中，非阻断、系统自救。
 *    Tier 2 中断页 —— 唯一阻断样式。仅两个入口：
 *                     ① 断线且自动重连超 30s；② 关键 HTTP 确认断网/重试耗尽。
 *                     页面上自动重试持续可见，唯一按钮 =「WiFi 设置」。
 *
 *  [PLATFORM] 标注的函数接实际工程；所有 ui_* 调用需切 UI 线程
 *  （原实现的 lv_async_call 模式原样沿用，此处省略以突出逻辑）。
 * ============================================================================
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ========================================================================== */
/*  1. 灯色与配置（OTA 可调；与总览 PRD 模块 B 的三态一致）                     */
/* ========================================================================== */

typedef enum { CL_GREEN = 0, CL_YELLOW = 1, CL_RED = 2 } class_light_t;

typedef struct {
    uint8_t  debounce_ticks;        /* 灯色防抖：连续 N 跳才切换，默认 3（≈6s）   */
    uint8_t  recover_ticks;         /* 恢复防抖：默认 3（沿用原实现取值）          */
    uint16_t drawer_cooldown_sec;   /* 抽屉冷却，默认 300（替代原 8 跳≈16s）       */
    uint8_t  drawer_cap_per_class;  /* 每课抽屉上限，默认 2                        */
    uint16_t reconnect_grace_sec;   /* 断线重试卡→中断页的宽限，默认 30            */
    uint8_t  http_silent_retry_max; /* 可延后请求静默重试上限，默认 2              */
} class_hint_cfg_t;

static class_hint_cfg_t g_chc = {
    .debounce_ticks = 3, .recover_ticks = 3,
    .drawer_cooldown_sec = 300, .drawer_cap_per_class = 2,
    .reconnect_grace_sec = 30, .http_silent_retry_max = 2,
};

/* TRTCQuality → 灯色映射（修 P4：POOR=3 归入黄档，消灭中立死区） */
static class_light_t quality_to_light(int q)
{
    if (q >= 5) return CL_RED;          /* VeryBad(5) / Down(6)                */
    if (q >= 3) return CL_YELLOW;       /* Poor(3) / Bad(4)                    */
    return CL_GREEN;                    /* Excellent(1) / Good(2)；Unknown(0)
                                           不改变当前灯色（在调用处过滤）       */
}

/* ========================================================================== */
/*  2. 状态（原 s_wn_manager 扩展；复位统一走 wn_reset_all，修 P9）             */
/* ========================================================================== */

typedef struct {
    /* 灯色防抖 */
    class_light_t shown, pending;
    uint8_t       pending_cnt;
    /* 抽屉限流 */
    uint8_t       drawer_cnt;           /* 本节课已弹次数（进课清零）           */
    uint32_t      drawer_last_sec;
    /* 方向归因（保存最近一次 local/remote，修 P5） */
    int           last_local_q, last_remote_q;
    /* 断线重试卡 → 中断页升级计时 */
    bool          reconnecting;
    uint32_t      retry_since_sec;
    /* 恢复抽屉只在「红过 / 断过」之后弹（避免黄一下就喊恢复） */
    bool          had_red_or_drop;
    /* Tier2 是否在屏（幂等保护，沿用原 is_showing 思想） */
    bool          interrupt_showing;
} class_hint_state_t;

static class_hint_state_t s_ch;

static void wn_reset_all(void)          /* 原 5 处散落复位块的统一出口          */
{
    memset(&s_ch, 0, sizeof(s_ch));
}

/* ---- 平台依赖 [PLATFORM]（全部需 lv_async_call 切 UI 线程） -------------- */
extern uint32_t plat_uptime_sec(void);
extern void ui_class_drawer(const char *text);   /* 穿透式底部抽屉（顶角 r40、底边出屏）：
                                     bars + 单行文案；升起 0.45s ease-out →
                                     停留 3s → 收回 0.45s ease-in               */
extern void ui_class_retry_show(const char *event_line,    /* 居中双行重试卡：   */
                                const char *progress_line); /* 事件行 + 蓝色进度行
                                     （spinner + 第 N 次外显，UI 侧随回调刷新）  */
extern void ui_class_retry_hide(void);
extern void ui_interrupt_page_show(bool is_disconnect);    /* Tier2 全屏中断页：
                                     自动重试可见，唯一按钮「WiFi 设置」；
                                     is_disconnect 决定归因文案（断网页 vs 弱网页，
                                     修 P3：弱网耗尽绝不进断网归因）             */
extern void ui_interrupt_page_hide(void);
extern void plat_play_hint_sound(void);                    /* 短音效，可设置关  */

/* ========================================================================== */
/*  3. 轻度抽屉：onNetworkQuality 驱动（原 notify_trtc_quality 的替代者）       */
/*     签名改为双通道（修 P5）；不再有 overlay、不再有假重试（修 P1/P2）        */
/* ========================================================================== */

static void drawer_once(const char *text)
{
    uint32_t now = plat_uptime_sec();
    if (s_ch.drawer_cnt >= g_chc.drawer_cap_per_class) return;      /* 修 P7 */
    if (now - s_ch.drawer_last_sec < g_chc.drawer_cooldown_sec) return;
    s_ch.drawer_cnt++; s_ch.drawer_last_sec = now;
    ui_class_drawer(text);
    plat_play_hint_sound();
}

/* 灯色下降沿 → 方向归因文案（上行差 = 你的画面；下行差 = 课程画面） */
static void on_light_downgrade(class_light_t to)
{
    if (to == CL_YELLOW) { drawer_once("网络一般 · 已降低画质"); return; }
    s_ch.had_red_or_drop = true;
    /* 红档文案只说方向——「较差」由抽屉内的红色 bars 表达，避免超出底部圆弦 */
    drawer_once(s_ch.last_local_q >= s_ch.last_remote_q
               ? "你的画面可能卡顿"
               : "课程画面可能卡顿");
}

void weak_network_notify_trtc_quality(int local_q, int remote_q)
{
    if (local_q == 0 && remote_q == 0) return;      /* Unknown：不动当前状态   */
    s_ch.last_local_q = local_q; s_ch.last_remote_q = remote_q;

    int worst = (local_q > remote_q) ? local_q : remote_q;
    class_light_t l = quality_to_light(worst);

    /* 防抖：恢复方向与恶化方向可用不同 tick 数（当前同为 3） */
    if (l != s_ch.pending) { s_ch.pending = l; s_ch.pending_cnt = 1; return; }
    uint8_t need = (l < s_ch.shown) ? g_chc.recover_ticks : g_chc.debounce_ticks;
    if (++s_ch.pending_cnt < need || l == s_ch.shown) return;

    class_light_t prev = s_ch.shown;
    s_ch.shown = l;

    /* 评审决议：课中无独立常驻角标——bars 永远随容器（抽屉/重试卡/阻断页）出现 */
    /* 轻度抽屉：仅下降沿弹一次；恢复到绿且此前红过/断过 → 恢复抽屉 */
    if (l > prev) {
        on_light_downgrade(l);
    } else if (l == CL_GREEN && s_ch.had_red_or_drop) {
        ui_class_drawer("网络已恢复");
        s_ch.had_red_or_drop = false;
    }
}

/* ========================================================================== */
/*  4. 断线链路：重试卡立即、中断页要等（修「onDisconnected 一次就跳断网页」）   */
/* ========================================================================== */

void weak_network_notify_trtc_disconnected(void)
{
    if (s_ch.interrupt_showing) return;
    s_ch.had_red_or_drop = true;
    if (!s_ch.reconnecting) {
        s_ch.reconnecting = true;
        s_ch.retry_since_sec = plat_uptime_sec();
        ui_class_retry_show("网络中断", "自动重连中…");     /* 非阻断，画面定格 */
    }
}

/* 挂 1s 周期 tick（课中即有）：重试卡超宽限 → 升级 Tier2 */
void class_hint_tick_1s(void)
{
    if (s_ch.reconnecting && !s_ch.interrupt_showing &&
        plat_uptime_sec() - s_ch.retry_since_sec > g_chc.reconnect_grace_sec) {
        ui_class_retry_hide();
        s_ch.interrupt_showing = true;
        ui_interrupt_page_show(true /* 真断线：断网归因 */);
    }
}

void weak_network_notify_trtc_connected(void)
{
    bool was_visible = s_ch.reconnecting || s_ch.interrupt_showing;
    if (s_ch.reconnecting)       ui_class_retry_hide();
    if (s_ch.interrupt_showing)  ui_interrupt_page_hide();
    s_ch.reconnecting = false;
    s_ch.interrupt_showing = false;
    if (was_visible) ui_class_drawer("网络已恢复");
    /* 灯色防抖状态保留（质量回调会自行修正），计数类不清——每课上限仍有效 */
}

/* ========================================================================== */
/*  5. HTTP 链路：分级 + 静默重试（原 pending_ctx 重放机制原样复用，修 P6）      */
/* ========================================================================== */

typedef enum {
    HTTP_REQ_DEFERRABLE = 0,   /* 打点/上报/组间上传：静默重试→本地缓存，永不上屏 */
    HTTP_REQ_CRITICAL   = 1,   /* 进课鉴权/课程资源：影响课程流程                  */
} http_req_class_t;

extern http_req_class_t http_classify(int mtype);   /* [PLATFORM] mtype 分级表  */
extern void http_replay_pending(void);               /* [PLATFORM] 复用原
                                pending_ctx + http_process_insert_msg 重放机制，
                                callback 指向本模块的结果回调                     */
extern void http_defer_to_local_cache(int mtype);    /* [PLATFORM] 恢复后补传    */

static uint8_t s_http_retry_cnt = 0;

/* 原 weak_network_notify_timeout 的替代者：超时不再直接弹 overlay */
void weak_network_notify_timeout(int mtype)
{
    if (http_classify(mtype) == HTTP_REQ_DEFERRABLE) {
        if (s_http_retry_cnt++ < g_chc.http_silent_retry_max) http_replay_pending();
        else { http_defer_to_local_cache(mtype); s_http_retry_cnt = 0; }
        return;                                       /* 后台请求永不上屏        */
    }
    /* 关键请求：第 1 次静默重试；第 2 次带重试卡重试；耗尽 → Tier2（弱网归因） */
    if (s_http_retry_cnt == 0) {
        s_http_retry_cnt = 1; http_replay_pending();
    } else if (s_http_retry_cnt == 1) {
        s_http_retry_cnt = 2;
        ui_class_retry_show("课程数据加载失败", "重试中（1/2）…");
        http_replay_pending();
    } else {
        s_http_retry_cnt = 0;
        ui_class_retry_hide();
        s_ch.interrupt_showing = true;
        ui_interrupt_page_show(false /* 弱网归因：连接在、质量差，修 P3 */);
    }
}

void weak_network_on_http_success(void)   /* 重放成功：收重试卡，计数清零 */
{
    s_http_retry_cnt = 0;
    ui_class_retry_hide();
}

void weak_network_notify_no_network(void) /* 确认断网：语义本来就对，保留 */
{
    s_http_retry_cnt = 0;
    ui_class_retry_hide();
    s_ch.interrupt_showing = true;
    ui_interrupt_page_show(true);
}

/* ========================================================================== */
/*  6. 课程生命周期                                                             */
/* ========================================================================== */

void class_hint_on_class_enter(void)
{
    wn_reset_all();                       /* 抽屉配额按课重置                   */
    /* 带宽保护（PRD-3 §3）：进课暂停后台大文件上传、取消排队队列 [PLATFORM] */
}

void class_hint_on_class_exit(void)
{
    wn_reset_all();
    ui_class_retry_hide();
    if (s_ch.interrupt_showing) ui_interrupt_page_hide();
}
