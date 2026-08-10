#!/bin/bash
# pKVM 模糊测试 —— 一体化操作脚本
#
#   ./delivery/pkvm-fuzz.sh check     开跑前的环境自检
#   ./delivery/pkvm-fuzz.sh start     推送二进制并启动测试
#   ./delivery/pkvm-fuzz.sh status    查看进度
#   ./delivery/pkvm-fuzz.sh report    生成报告
#   ./delivery/pkvm-fuzz.sh stop      停止测试
#
# 用法与结果解读见同目录的 README.md。

set -uo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
CFG=${CFG:-$REPO/delivery/n90-delivery.cfg}
BOARD=${BOARD:-root@10.42.27.17}
KLINUX=${KLINUX:-/home/jose/klinux}
COV=/sys/kernel/debug/kvm/pkvm_cov

# workdir 从配置里读，避免脚本和配置各说各话
WORKDIR=$(sed 's/#.*//' "$CFG" | python3 -c 'import json,sys; print(json.load(sys.stdin)["workdir"])' 2>/dev/null)

ok()   { printf '  \033[32m✓\033[0m %s\n' "$*"; }
bad()  { printf '  \033[31m✗\033[0m %s\n' "$*"; FAIL=1; }
warn() { printf '  \033[33m!\033[0m %s\n' "$*"; }
rsh()  { timeout 30 ssh -o ConnectTimeout=10 -o BatchMode=yes "$BOARD" "$1" 2>/dev/null; }

cmd_check() {
	FAIL=0
	echo "环境自检（目标：$BOARD）"

	rsh 'echo ok' >/dev/null && ok "板子可达" || { bad "板子不可达 —— ssh 不通"; return 1; }

	# 内核必须带 EL2 覆盖率桥，否则测得到 host 但看不见 hypervisor
	if rsh "test -r $COV/stats"; then
		ok "内核带 EL2 覆盖率桥（$COV 存在）"
	else
		bad "内核没有 EL2 覆盖率桥：$COV 不存在。需要 CONFIG_PKVM_EL2_COV=y 的内核"
	fi

	grep -q 'kvm-arm.mode=protected' <(rsh 'cat /proc/cmdline') \
		&& ok "内核以 kvm-arm.mode=protected 启动" \
		|| bad "内核不是 protected 模式启动 —— pKVM 路径不会被执行"

	local armed leaked
	armed=$(rsh "awk '/^armed_cpus/{print \$2}' $COV/stats")
	leaked=$(rsh "awk '/^leaked_bytes/{print \$2}' $COV/stats")
	[ "${armed:-0}" -gt 0 ] 2>/dev/null \
		&& ok "覆盖率 ring 已武装（$armed 个 CPU）" \
		|| bad "覆盖率 ring 未武装（armed_cpus=${armed:-?}）。执行：systemctl start pkvm-cov-arm"
	# 非 0 说明有内存块因 EL2 可能仍在映射而被永久保留，重启才能回收
	[ "${leaked:-0}" = 0 ] \
		&& ok "无泄漏内存（leaked_bytes=0）" \
		|| warn "leaked_bytes=$leaked —— 之前的 ring 拆除未确认，建议重启板子"

	# 基线必须干净：开跑前就有的 WARN 会和测试结果混在一起，事后分不开
	local noise
	noise=$(rsh 'dmesg | grep -icE "WARNING:|BUG:|soft lockup"')
	if [ "${noise:-0}" -le 3 ]; then
		ok "dmesg 基线干净（${noise:-?} 条，厂商开机噪声）"
	else
		warn "dmesg 已有 ${noise} 条 WARNING/BUG —— 建议重启，否则新旧告警混淆"
	fi

	[ -x "$REPO/bin/syz-manager" ] && ok "syz-manager 已构建" || bad "缺少 bin/syz-manager，执行 make manager"
	[ -x "$REPO/bin/linux_arm64/syz-executor" ] && ok "executor 已构建" \
		|| bad "缺少 bin/linux_arm64/syz-executor，执行 make TARGETOS=linux TARGETARCH=arm64 executor"

	# manager 与 executor 版本不一致时，要等 RPC 握手后才报错，现象像“程序卡住”
	local rev
	rev=$(cd "$REPO" && git rev-parse HEAD)
	# grep -c，不用 grep -q：-q 命中后立即退出，strings 收到 SIGPIPE 返回 141，
	# 而 set -o pipefail 会把整条管道判为失败 —— 于是这条检查在版本正确时反而报错，
	# 挡住每一次合法运行。
	local hit
	hit=$(strings "$REPO/bin/linux_arm64/syz-executor" 2>/dev/null | grep -cE "^${rev}\+?$")
	[ "${hit:-0}" -gt 0 ] \
		&& ok "executor 与 HEAD 同版本" \
		|| bad "executor 不是从当前 HEAD 构建的 —— 版本不一致会在握手后才报错，先重新构建"

	echo
	[ "${FAIL:-0}" = 0 ] && echo "自检通过，可以 start。" || echo "自检未通过，请先处理上面的 ✗。"
	return "${FAIL:-0}"
}

cmd_start() {
	cmd_check || { echo "已中止。"; return 1; }
	echo
	echo "推送板子端二进制…"
	scp -q "$REPO/bin/linux_arm64/syz-executor" "$REPO/bin/linux_arm64/syz-execprog" "$BOARD:/tmp/" || {
		echo "推送失败"; return 1; }
	rsh 'chmod +x /tmp/syz-executor /tmp/syz-execprog'

	mkdir -p "$WORKDIR"
	if pgrep -x syz-manager >/dev/null; then
		echo "已有 syz-manager 在运行，先执行 stop。"; return 1
	fi
	( cd "$REPO" && nohup ./bin/syz-manager -config "$CFG" > "$WORKDIR/manager.log" 2>&1 & )
	sleep 5
	pgrep -x syz-manager >/dev/null \
		&& echo "已启动。日志：$WORKDIR/manager.log    网页：http://localhost:56760" \
		|| { echo "启动失败，看 $WORKDIR/manager.log"; return 1; }
}

cmd_status() {
	pgrep -x syz-manager >/dev/null && echo "syz-manager：运行中（已运行 $(ps -o etime= -p "$(pgrep -x syz-manager)" | tr -d ' ')）" \
		|| echo "syz-manager：未运行"
	echo
	echo "最近进度："
	grep -v 'kernel build has changed' "$WORKDIR/manager.log" 2>/dev/null | grep 'exec total' | tail -3 | sed 's/^/  /'
	echo
	local n
	n=$(ls "$WORKDIR/crashes" 2>/dev/null | wc -l)
	echo "崩溃数：$n"
	if [ "$n" -gt 0 ]; then
		# 交付配置下的预期是 0；出现即需要人看，所以直接把标题列出来
		for d in "$WORKDIR"/crashes/*/; do
			[ -r "$d/description" ] && echo "  - $(cat "$d/description")"
		done
	fi
	echo
	echo "板子："
	rsh 'uptime' | sed 's/^/  /'
	rsh "grep -E '^(armed_cpus|leaked_bytes|LOST_IN_RING)' $COV/stats" | sed 's/^/  /'
}

cmd_report() {
	[ -d "$WORKDIR" ] || { echo "找不到 workdir：$WORKDIR"; return 1; }
	python3 "$REPO/delivery/make-report.py" \
		--workdir "$WORKDIR" \
		--vmlinux "$KLINUX/vmlinux" \
		--klinux "$KLINUX" \
		--board "$BOARD" \
		--config "$CFG"
	echo "报告：$WORKDIR/report/report.md"
}

cmd_stop() {
	# -x 按进程名精确匹配：-f 会匹配到本脚本自己的命令行，把自己也杀掉
	pkill -x syz-manager && echo "已停止。" || echo "没有在运行的 syz-manager。"
}

case "${1:-}" in
check)  cmd_check ;;
start)  cmd_start ;;
status) cmd_status ;;
report) cmd_report ;;
stop)   cmd_stop ;;
*)      sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//' ;;
esac
