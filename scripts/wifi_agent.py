#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""AMLgsMenu ConnMan Agent.

设备上的 Wi-Fi 由 ConnMan 独占管理，AMLgsMenu 只通过它在 D-Bus 上的
正规通道操作：

  * 扫描/枚举  : Technology.Scan / Manager.GetServices
  * 连接       : Service.Connect
  * 密码       : 本文件扮演 net.connman.Agent，应答 RequestInput

为什么必须有 Agent：ConnMan 1.43 的 service_connect() 对 WPA-PSK/RSN 服务，
在 service->passphrase 为空时直接返回 -ENOKEY，随后只会向 Agent 发
RequestInput 索要密码。没有 Agent 时 State 恒为 idle，连一条 association
都不会发生（已在地面端实测确认）。

本脚本不启动、不干扰 wpa_supplicant —— ConnMan 自己用 -u 模式持有它，
我们只是 D-Bus 上的一个应答者。

与 CoreELEC 官方网络插件（service.coreelec.settings）保持同一模型：
Connect 不阻塞事件循环。Service.Connect 的 D-Bus 回复要等连接成功或失败
才发（ConnMan 的 CONNECT_TIMEOUT 是 120s），而 RequestInput 的应答必须由
同一个事件循环发出。之前把 Connect 放进工作线程同步等待，解释器被占住，
RequestInput 的回复要等那次调用 25s 客户端超时之后才发得出去，ConnMan
拿到密码之前一直是 idle。真机抓包（dbus-monitor）确认过这一点：
Connect 45.545 -> RequestInput 45.555 -> 回复 10.546（25s 后）-> AddNetwork 10.547。

用法（由 AMLgsMenu 调用，密码只走环境变量，绝不进 argv）：

    AMLGS_WIFI_PSK=xxx AMLGS_WIFI_SVC=wifi_xxx \\
        python3 /storage/digitalfpv/wifi_agent.py

可选环境变量：
    AMLGS_WIFI_PSK       要应答的密码（必填；为空则不做任何事，直接退出 2）
    AMLGS_AGENT_TIMEOUT  兜底寿命秒数（默认 130 = ConnMan 120s 连接上限 + 余量）。
                         正常情况下 Connect 一有结果本脚本就自行退出，
                         上层结束连接流程时也会 kill 它。
    AMLGS_AGENT_PATH     Agent 对象路径（默认 /amlgs/agent）
    AMLGS_WIFI_SVC       要连接的 ConnMan service 路径（如
                         wifi_xxx_managed_psk）。非空时由 Agent 自己
                         用同一条 D-Bus 连接发 Service.Connect。

退出码：
    0  正常退出（Connect 已有结果、收到 SIGTERM/SIGINT，或超时）
    2  没有可用的密码，未注册
    3  注册 Agent 失败
"""

import os
import signal
import sys
import time

AGENT_PATH = os.environ.get("AMLGS_AGENT_PATH", "/amlgs/agent")
PSK = os.environ.get("AMLGS_WIFI_PSK", "")
TIMEOUT = int(os.environ.get("AMLGS_AGENT_TIMEOUT", "130"))
SVC = os.environ.get("AMLGS_WIFI_SVC", "")
RESULT = os.environ.get("AMLGS_AGENT_RESULT", "/run/amlgs_wifi_agent.result")

# ConnMan 在连接失败时只把原因告诉 Agent（ReportError，随后给 Connect 回一个
# 很笼统的错误），然后把 service 置回 idle、不进 failure。所以轮询 State 的
# 上层看不到失败原因，必须由这里写出来：
#   error=<ConnMan 的 Error 码>   连接失败，ReportError 给出的原因
#   failed=<D-Bus 错误名>         Connect 请求本身被拒绝
#   ok                            Connect 正常返回（已连接）
def report(text):
    try:
        with open(RESULT, "w") as f:
            f.write(text + "\n")
    except Exception as exc:  # noqa: BLE001 - 只影响上层拿到原因的详细程度
        log("cannot write result file: %s" % exc)

# 毫秒。必须大于 ConnMan 的 CONNECT_TIMEOUT(120s)，否则我们这边先放弃时
# ConnMan 那边的连接还在进行，Agent 一走密码请求就没人应答。
CONNECT_TIMEOUT_MS = 125000

T_START = time.time()


def log(msg):
    print("[wifi_agent] t+%.3f %s" % (time.time() - T_START, msg), flush=True)


def main():
    if not PSK:
        log("AMLGS_WIFI_PSK is empty; nothing to answer, exit")
        return 2

    # dbus-python 与 GLib 主循环：ConnMan 调用 RequestInput 时必须有事件循环在跑
    import dbus
    import dbus.service
    import dbus.mainloop.glib
    from gi.repository import GLib

    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)

    try:
        bus = dbus.SystemBus()
    except Exception as exc:  # noqa: BLE001 - 起不来就只能退出，让上层报错
        log("cannot connect to system bus: %s" % exc)
        return 3

    class Agent(dbus.service.Object):
        """只应答 Passphrase；其它请求明确取消，便于上层排障。"""

        @dbus.service.method("net.connman.Agent",
                             in_signature="oa{sv}",
                             out_signature="a{sv}")
        def RequestInput(self, path, fields):
            keys = list(fields.keys())
            log("RequestInput %s -> %s" % (path, keys))
            if "Passphrase" in fields:
                log("answering Passphrase (len %d)" % len(PSK))
                return dbus.Dictionary({"Passphrase": dbus.String(PSK)},
                                       signature="sv")
            # 需要 Identity / WPS / WISPr 等我们没有的凭据：
            # 回 Canceled，让 ConnMan 立刻失败而不是等到 120s 超时。
            raise dbus.exceptions.DBusException(
                "no credentials available",
                name="net.connman.Agent.Error.Canceled")

        @dbus.service.method("net.connman.Agent", in_signature="", out_signature="")
        def Release(self):
            log("Release")

        @dbus.service.method("net.connman.Agent", in_signature="", out_signature="")
        def Cancel(self):
            log("Cancel")

        @dbus.service.method("net.connman.Agent", in_signature="os", out_signature="")
        def ReportError(self, path, error):
            # ConnMan 在连接失败时先调这里，随后才给 Connect 回错误。
            # 正常返回 = 不重试，service 回到 idle 并保留 Error=<error>。
            log("ReportError %s %s" % (path, error))
            report("error=%s" % error)

        @dbus.service.method("net.connman.Agent", in_signature="os", out_signature="")
        def RequestBrowser(self, path, url):
            log("RequestBrowser %s ignored" % path)

    agent = Agent(bus, AGENT_PATH)

    manager = dbus.Interface(bus.get_object("net.connman", "/"),
                             "net.connman.Manager")
    try:
        manager.RegisterAgent(dbus.ObjectPath(AGENT_PATH))
    except Exception as exc:  # noqa: BLE001
        log("RegisterAgent failed: %s" % exc)
        return 3
    log("registered at %s" % AGENT_PATH)

    loop = GLib.MainLoop()
    unregistered = {"done": False}

    def unregister():
        if unregistered["done"]:
            return
        unregistered["done"] = True
        try:
            manager.UnregisterAgent(dbus.ObjectPath(AGENT_PATH))
            log("unregistered")
        except Exception as exc:  # noqa: BLE001 - 退出路径，尽力而为
            log("UnregisterAgent failed (ignored): %s" % exc)

    def finish(reason):
        log("finish: %s" % reason)
        unregister()
        loop.quit()

    def on_signal(signum, _frame):
        log("signal %d received, shutting down" % signum)
        unregister()
        loop.quit()

    # 关键：Connect 必须由本 Agent 自己的 D-Bus 连接发出。
    # ConnMan 按 Connect 调用者的 sender 查 agent，查不到才退到
    # default_agent。若由外部 dbus-send 发 Connect，就会落到
    # default_agent；一旦 CoreELEC 设置插件的 agent 也注册着，
    # 密码请求会被它接走并弹出 Kodi 键盘。
    #
    # 同样关键：Connect 不能阻塞。它的回复要等连接结束才来，必须异步发，
    # 让 GLib 主循环空闲着去应答 RequestInput。
    if SVC:
        svc_obj = bus.get_object("net.connman",
                                 "/net/connman/service/" + SVC)
        svc_iface = dbus.Interface(svc_obj, "net.connman.Service")

        def on_connect_reply(*_args):
            # 空 method return = 连接成功（State 已到 ready）
            report("ok")
            finish("Connect returned (connected)")

        def on_connect_error(exc):
            get_name = getattr(exc, "get_dbus_name", None)
            name = get_name() if callable(get_name) else type(exc).__name__
            if not os.path.exists(RESULT):
                # 没有 ReportError 就直接被拒：请求本身没被受理（InProgress 等）
                report("failed=%s" % name)
            finish("Connect error: %s: %s" % (name, exc))

        log("calling Connect (async) on %s" % SVC)
        svc_iface.Connect(reply_handler=on_connect_reply,
                          error_handler=on_connect_error,
                          timeout=CONNECT_TIMEOUT_MS)

    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)

    # 兜底寿命：上层（AMLgsMenu）会在连接结束后 kill 我们，
    # 但万一上层异常退出，这里也不会留下永久常驻的进程。
    GLib.timeout_add_seconds(TIMEOUT, lambda: (finish("self timeout %ds" % TIMEOUT), False)[-1])

    try:
        loop.run()
    finally:
        unregister()
    return 0


if __name__ == "__main__":
    sys.exit(main())
