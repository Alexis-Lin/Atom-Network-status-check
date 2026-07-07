/**
 * @file weak_network_manager.c
 * @brief workout 弱网检测与重试管理器
 *
 * 当 http_workout.c 中的请求经过 http_execute_request_with_retry() 重试3次后
 * 仍然超时，通过本模块弹出 overlay 提示页面，允许用户手动重试最多2次；
 * 若仍失败则跳转至 wifi_poor_connection_page。
 */

#include <string.h>
#include "weak_network_manager.h"
#include "http_common.h"
#include "page_switch.h"
#include "base/Log.h"
#include "lvgl/lvgl.h"
#include "weak_network_overlay.h"

// ============================================================================
// 内部状态
// ============================================================================

static weak_network_manager_t s_wn_manager = {
    .is_showing           = false,
    .trtc_triggered       = false,
    .trtc_bad_count       = 0,
    .trtc_good_count      = 0,
    .trtc_retry_count     = 0,
    .trtc_retry_cooldown  = 0,
    .trtc_disconnect_count = 0,
    .pending_ctx          = {0}
};

static PageState s_pre_page = PAGE_WORKOUT;

static void weak_network_retry_callback(void* data);
static void weak_network_on_retry_clicked(void);
static void weak_network_switch_pre_page(void);

static void show_weak_network_overlay_cb(void* data) {
    (void)data;
    weak_network_overlay_show(weak_network_on_retry_clicked, weak_network_switch_pre_page);
}

static void hide_weak_network_overlay_cb(void* data) {
    (void)data;
    weak_network_overlay_hide();
}

static void goto_wifi_poor_connection_cb(void* data) {
    (void)data;
    weak_network_overlay_hide();
    switch_page(PAGE_WIFI_POOR_CONNECTION, &s_pre_page);
}

static void goto_wifi_no_connection_cb(void* data) {
    (void)data;
    weak_network_overlay_hide();
    switch_page(PAGE_WIFI_NOT_CONNECT, &s_pre_page);
}

static void weak_network_switch_pre_page(void) {
    weak_network_overlay_hide();
    switch_page(s_pre_page, NULL);
}

// ============================================================================
// 内部重试回调（在 httpProcess 后台线程执行）
// ============================================================================

static void weak_network_retry_callback(void* data) {
    if (!data) return;

    int ret = *(int*)data;

    if (ret != (int)HTTP_ERROR_TIMEOUT) {
        // 断网：直接跳转 PAGE_WIFI_POOR_CONNECTION，不调用原始 callback
        if (http_is_network_disconnected(ret)) {
            s_wn_manager.is_showing      = false;
            s_wn_manager.trtc_triggered  = false;
            s_wn_manager.trtc_bad_count  = 0;
            s_wn_manager.trtc_good_count = 0;
            lv_async_call(goto_wifi_poor_connection_cb, NULL);
            return;
        }
        // 重试成功或其他非断网错误：清除状态，隐藏 overlay，调用原始 callback
        http_process_callback original_cb = s_wn_manager.pending_ctx.original_callback;
        s_wn_manager.is_showing      = false;
        s_wn_manager.trtc_triggered  = false;
        s_wn_manager.trtc_bad_count  = 0;
        s_wn_manager.trtc_good_count = 0;
        lv_async_call(hide_weak_network_overlay_cb, NULL);
        if (original_cb != NULL) {
            original_cb(data);
        }
        return;
    }

    // 仍然超时
    s_wn_manager.pending_ctx.retry_count++;
    TRACEF("weak_network retry_count: %d\n", s_wn_manager.pending_ctx.retry_count);

    if (s_wn_manager.pending_ctx.retry_count >= 2) {
        // 重试耗尽，跳转弱网页面
        s_wn_manager.is_showing      = false;
        s_wn_manager.trtc_triggered  = false;
        s_wn_manager.trtc_bad_count  = 0;
        s_wn_manager.trtc_good_count = 0;
        lv_async_call(goto_wifi_poor_connection_cb, NULL);
    } else {
        // 还有重试机会，重新显示 overlay
        s_wn_manager.is_showing = true;
        lv_async_call(show_weak_network_overlay_cb, NULL);
    }
}

// ============================================================================
// 公开接口实现
// ============================================================================

void weak_network_set_pre_page(PageState pre_page) {
    s_pre_page = pre_page;
}

bool weak_network_is_retry_callback(http_process_callback cb) {
    return cb == weak_network_retry_callback;
}

void weak_network_notify_timeout(const http_msg_queue_t* original_msg) {
    if (!original_msg) return;

    // 幂等保护：已在显示则忽略
    if (s_wn_manager.is_showing) {
        TRACEF("weak_network overlay already showing, ignore\n");
        return;
    }

    // 保存请求上下文
    s_wn_manager.pending_ctx.msg_type          = original_msg->mtype;
    s_wn_manager.pending_ctx.normal_param      = original_msg->normal_param;
    s_wn_manager.pending_ctx.original_callback = original_msg->callback;
    s_wn_manager.pending_ctx.retry_count       = 0;
    s_wn_manager.is_showing                    = true;
    s_wn_manager.trtc_triggered                = false;  // HTTP 触发，非 TRTC

    TRACEF("weak_network timeout detected, mtype=%d\n", original_msg->mtype);

    // 切换到 UI 线程显示 overlay
    lv_async_call(show_weak_network_overlay_cb, NULL);
}

static void weak_network_on_retry_clicked(void) {
    if (!s_wn_manager.is_showing) return;

    // 重建消息并重新入队，callback 替换为内部重试回调
    http_msg_queue_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.mtype        = s_wn_manager.pending_ctx.msg_type;
    msg.normal_param = s_wn_manager.pending_ctx.normal_param;
    msg.callback     = weak_network_retry_callback;

    TRACEF("weak_network retry clicked, mtype=%d retry_count=%d\n",
           msg.mtype, s_wn_manager.pending_ctx.retry_count);

    http_process_insert_msg(&msg);
}

void weak_network_notify_no_network(const http_msg_queue_t* original_msg) {
    TRACEF("weak_network no_network detected, mtype=%d\n",
           original_msg ? original_msg->mtype : -1);
    // 若 overlay 正在显示，goto_wifi_no_connection_cb 内部会隐藏它
    s_wn_manager.is_showing      = false;
    s_wn_manager.trtc_triggered  = false;
    s_wn_manager.trtc_bad_count  = 0;
    s_wn_manager.trtc_good_count = 0;
    lv_async_call(goto_wifi_no_connection_cb, NULL);
}

// ============================================================================
// TRTC 回调驱动的弱网/断网通知
// ============================================================================

/* 防抖阈值 */
#define TRTC_WEAK_TRIGGER_COUNT    2   // 连续 N 次 quality>=BAD 才触发弱网
#define TRTC_GOOD_RECOVER_COUNT    3   // 连续 N 次 quality<=GOOD 才自动恢复
#define TRTC_QUALITY_BAD_THRESH    4   // NetworkQuality: BAD=4, VERY_BAD=5, DOWN=6
#define TRTC_QUALITY_GOOD_THRESH   2   // NetworkQuality: EXCELLENT=1, GOOD=2
/* OnNetworkQuality 约 2s 回调一次，15s ≈ 7~8 次，取 8 */
#define TRTC_RETRY_COOLDOWN_TICKS  8   // retry 后冷却期（单位：quality 回调次数）
#define TRTC_RETRY_MAX             2   // TRTC 弱网最多 retry 次数
#define TRTC_DISCONNECT_MAX        1   // 连续 DISCONNECTED 达此次数才跳转断网页

static void weak_network_trtc_retry_clicked(void);

static void trtc_show_weak_overlay_cb(void* data) {
    (void)data;
    // TRTC 触发的弱网 overlay 有 Retry 按钮，回调走 trtc 专用 retry 路径
    weak_network_overlay_show(weak_network_trtc_retry_clicked, weak_network_switch_pre_page);
}

static void trtc_hide_overlay_cb(void* data) {
    (void)data;
    weak_network_overlay_hide();
}

/* TRTC 弱网 overlay 的 Retry 按钮回调（UI 线程） */
static void weak_network_trtc_retry_clicked(void) {
    if (!s_wn_manager.is_showing || !s_wn_manager.trtc_triggered) return;

    s_wn_manager.trtc_retry_count++;
    TRACEF("weak_network trtc retry clicked, retry_count=%d\n",
           s_wn_manager.trtc_retry_count);

    // 无论是第几次 retry，都隐藏 overlay 并进入冷却期
    // 冷却结束后：
    //   retry_count < TRTC_RETRY_MAX → 再弹一次 overlay
    //   retry_count >= TRTC_RETRY_MAX → 直接跳转断网页
    s_wn_manager.is_showing          = false;
    s_wn_manager.trtc_retry_cooldown = TRTC_RETRY_COOLDOWN_TICKS;
    weak_network_overlay_hide();
}

void weak_network_notify_trtc_quality(int quality) {
    if (quality >= TRTC_QUALITY_BAD_THRESH) {
        // 弱网方向：清除恢复计数，累加弱网计数
        s_wn_manager.trtc_good_count = 0;
        s_wn_manager.trtc_bad_count++;

        // 冷却期倒计时
        if (s_wn_manager.trtc_retry_cooldown > 0) {
            s_wn_manager.trtc_retry_cooldown--;
            TRACEF("weak_network trtc quality=%d bad_count=%d cooldown=%d\n",
                   quality, s_wn_manager.trtc_bad_count,
                   s_wn_manager.trtc_retry_cooldown);
            return;  // 冷却期内不触发任何动作
        }

        // TRACEF("weak_network trtc quality=%d bad_count=%d retry_count=%d\n",
        //        quality, s_wn_manager.trtc_bad_count, s_wn_manager.trtc_retry_count);

        if (s_wn_manager.trtc_bad_count >= TRTC_WEAK_TRIGGER_COUNT
                && !s_wn_manager.is_showing) {
            if (s_wn_manager.trtc_retry_count >= TRTC_RETRY_MAX) {
                // 第2次 retry 后冷却结束，仍然弱网 → 直接跳转断网页
                TRACEF("weak_network trtc retry exhausted after cooldown, goto no_connection\n");
                s_wn_manager.is_showing            = false;
                s_wn_manager.trtc_triggered        = false;
                s_wn_manager.trtc_bad_count        = 0;
                s_wn_manager.trtc_good_count       = 0;
                s_wn_manager.trtc_retry_count      = 0;
                s_wn_manager.trtc_retry_cooldown   = 0;
                s_wn_manager.trtc_disconnect_count = 0;
                lv_async_call(goto_wifi_no_connection_cb, NULL);
            } else {
                // 还有 retry 机会，弹出 overlay
                s_wn_manager.is_showing     = true;
                s_wn_manager.trtc_triggered = true;
                TRACEF("weak_network trtc triggered overlay (retry_count=%d)\n",
                       s_wn_manager.trtc_retry_count);
                lv_async_call(trtc_show_weak_overlay_cb, NULL);
            }
        }
    } else if (quality <= TRTC_QUALITY_GOOD_THRESH && quality > 0) {
        // 恢复方向：清除弱网计数和冷却，累加恢复计数
        s_wn_manager.trtc_bad_count      = 0;
        s_wn_manager.trtc_retry_cooldown = 0;
        s_wn_manager.trtc_good_count++;
        // TRACEF("weak_network trtc quality=%d good_count=%d\n",
        //        quality, s_wn_manager.trtc_good_count);

        if (s_wn_manager.trtc_good_count >= TRTC_GOOD_RECOVER_COUNT
                && s_wn_manager.is_showing
                && s_wn_manager.trtc_triggered) {
            // 仅自动恢复 TRTC 触发的 overlay
            s_wn_manager.is_showing           = false;
            s_wn_manager.trtc_triggered       = false;
            s_wn_manager.trtc_good_count      = 0;
            s_wn_manager.trtc_retry_count     = 0;
            s_wn_manager.trtc_retry_cooldown  = 0;
            TRACEF("weak_network trtc recovered, hiding overlay\n");
            lv_async_call(trtc_hide_overlay_cb, NULL);
        }
    } else {
        // POOR(3) 或 UNKNOWN(0)：中间态，两个计数器清零，保持当前状态
        s_wn_manager.trtc_bad_count  = 0;
        s_wn_manager.trtc_good_count = 0;
        TRACEF("weak_network trtc quality=%d (neutral), reset counters\n", quality);
    }
}

void weak_network_notify_trtc_disconnected(void) {
    s_wn_manager.trtc_disconnect_count++;
    TRACEF("weak_network trtc disconnected, disconnect_count=%d\n",
           s_wn_manager.trtc_disconnect_count);

    if (s_wn_manager.trtc_disconnect_count >= TRTC_DISCONNECT_MAX) {
        // 连续断开达阈值，SDK 自动重连均失败，跳转断网页
        TRACEF("weak_network trtc disconnect threshold reached, goto no_connection\n");
        s_wn_manager.is_showing            = false;
        s_wn_manager.trtc_triggered        = false;
        s_wn_manager.trtc_bad_count        = 0;
        s_wn_manager.trtc_good_count       = 0;
        s_wn_manager.trtc_retry_count      = 0;
        s_wn_manager.trtc_retry_cooldown   = 0;
        s_wn_manager.trtc_disconnect_count = 0;
        lv_async_call(goto_wifi_no_connection_cb, NULL);
    }
}

void weak_network_notify_trtc_connected(void) {
    TRACEF("weak_network trtc connected, reset disconnect_count\n");
    // 重连成功，清除断开计数和弱网状态
    s_wn_manager.trtc_disconnect_count = 0;
    s_wn_manager.trtc_bad_count        = 0;
    s_wn_manager.trtc_good_count       = 0;
    s_wn_manager.trtc_retry_cooldown   = 0;

    if (s_wn_manager.is_showing && s_wn_manager.trtc_triggered) {
        s_wn_manager.is_showing       = false;
        s_wn_manager.trtc_triggered   = false;
        s_wn_manager.trtc_retry_count = 0;
        lv_async_call(trtc_hide_overlay_cb, NULL);
    }
}
