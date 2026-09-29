// Copyright 2024 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package rpcserver

import (
	"bytes"
	"debug/elf"
	"errors"
	"fmt"
	"os"
	"slices"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/google/syzkaller/pkg/cover"
	"github.com/google/syzkaller/pkg/flatrpc"
	"github.com/google/syzkaller/pkg/fuzzer/queue"
	"github.com/google/syzkaller/pkg/log"
	"github.com/google/syzkaller/pkg/osutil"
	"github.com/google/syzkaller/pkg/report"
	"github.com/google/syzkaller/pkg/stat"
	"github.com/google/syzkaller/prog"
	"github.com/google/syzkaller/sys/targets"
)

type Runner struct {
	id              int
	source          *queue.Distributor
	procs           int
	cover           bool
	coverEdges      bool
	filterSignal    bool
	debug           bool
	debugTimeouts   bool
	progTarget      *prog.Target
	sysTarget       *targets.Target
	stats           *runnerStats
	finished        chan bool
	injectExec      chan<- bool
	infoc           chan chan []byte
	canonicalizer   *cover.CanonicalizerInstance
	nextRequestID   int64
	requests        map[int64]*queue.Request
	executing       map[int64]bool
	hanged          map[int64]bool
	lastExec        *LastExecuting
	updInfo         UpdateInfo
	resultCh        chan error
	lastRequestTime time.Time
	// xh drains EL2 translation-block PCs from the QEMU TCG plugin shm.
	// Nil unless cfg.XHyperCover is set.
	xh *xhDrainer
	// xhMgr drains the resource manager's EL1 window. Nil unless the plugin
	// was given that window; its blocks merge into the same signal set,
	// their address range being disjoint from the hypervisor's.
	xhMgr *xhDrainer
	// xhBootMask holds every block the hypervisor executed while this VM was
	// coming up, before any program ran. Set once per VM lifecycle from the
	// first drained window and then subtracted from every later window's
	// SIGNAL (never from its coverage). See the comment at the drain site.
	xhBootMask map[uint64]bool
	// Counters behind xhNoteProgram; only touched on the connection's own
	// goroutine, like xhBootMask.
	xhProgs      int
	xhProgsNoMgr int
	xhMgrHist    [len(xhMgrBuckets) + 1]int
	xhStarted    int
	// Resolved once per runner from the manager ELF; empty when not configured.
	xhWayRanges []xhRange
	xhWayHits   [len(xhWaypoints)]int
	// Counted per distinct combination of waypoints, not per waypoint: a
	// stage can be skipped legitimately (a document asking for its device
	// tree to be preserved never installs a projection), so independent
	// per-stage totals cannot tell a skipped stage from a failed one. The
	// path a program took can.
	xhWayPaths map[uint32]int
	// Outcomes taken from the program's own return values, not from coverage.
	xhSetupOK, xhSetupFail int
	xhStartOK, xhStartFail int
	// Why a start failed, keyed by the errno the program itself received. The
	// waypoints can only say which function was entered; the errno is the only
	// signal that says what it returned, and the failures split into causes
	// that live at different places in the Manager (a projection slice out of
	// range, an image that would not sync, a missing boot vCPU, an entry
	// address the vCPU refused). Without this the 290 failures are one lump.
	xhStartErrno map[int32]int

	// The mutex protects all the fields below.
	mu          sync.Mutex
	conn        *flatrpc.Conn
	stopped     bool
	machineInfo []byte
}

type runnerStats struct {
	statExecs              *stat.Val
	statExecRetries        *stat.Val
	statExecutorRestarts   *stat.Val
	statExecBufferTooSmall *stat.Val
	statNoExecRequests     *stat.Val
	statNoExecDuration     *stat.Val
}

type handshakeConfig struct {
	VMLess     bool
	Timeouts   targets.Timeouts
	LeakFrames []string
	RaceFrames []string
	Files      []string
	Features   flatrpc.Feature

	// Callback() is called in the middle of the handshake process.
	// The return arguments are the coverage filter and the (possible) error.
	Callback func(*flatrpc.InfoRequestRawT) (handshakeResult, error)
}

type handshakeResult struct {
	Files         []*flatrpc.FileInfo
	Features      []*flatrpc.FeatureInfo
	CovFilter     []uint64
	MachineInfo   []byte
	Canonicalizer *cover.CanonicalizerInstance
}

func (runner *Runner) Handshake(conn *flatrpc.Conn, cfg *handshakeConfig) (handshakeResult, error) {
	if runner.updInfo != nil {
		runner.updInfo(func(info *RunnerInfo) {
			info.Status = "handshake"
		})
	}

	connectReply := &flatrpc.ConnectReply{
		Debug:            runner.debug,
		Cover:            runner.cover,
		CoverEdges:       runner.coverEdges,
		Kernel64Bit:      runner.sysTarget.PtrSize == 8,
		Procs:            int32(runner.procs),
		Slowdown:         int32(cfg.Timeouts.Slowdown),
		SyscallTimeoutMs: int32(cfg.Timeouts.Syscall / time.Millisecond),
		ProgramTimeoutMs: int32(cfg.Timeouts.Program / time.Millisecond),
		LeakFrames:       cfg.LeakFrames,
		RaceFrames:       cfg.RaceFrames,
		Files:            cfg.Files,
		Features:         cfg.Features,
	}
	if err := flatrpc.Send(conn, connectReply); err != nil {
		return handshakeResult{}, err
	}
	infoReq, err := flatrpc.Recv[*flatrpc.InfoRequestRaw](conn)
	if err != nil {
		return handshakeResult{}, err
	}
	ret, err := cfg.Callback(infoReq)
	if err != nil {
		return handshakeResult{}, err
	}
	infoReply := &flatrpc.InfoReply{
		CoverFilter: ret.CovFilter,
	}
	if err := flatrpc.Send(conn, infoReply); err != nil {
		return handshakeResult{}, err
	}
	runner.mu.Lock()
	runner.conn = conn
	runner.machineInfo = ret.MachineInfo
	runner.canonicalizer = ret.Canonicalizer
	runner.mu.Unlock()

	if runner.updInfo != nil {
		runner.updInfo(func(info *RunnerInfo) {
			info.MachineInfo = runner.MachineInfo
			info.DetailedStatus = runner.QueryStatus
		})
	}
	return ret, nil
}

// xhDrainAll takes both coverage windows. The two ranges are disjoint, so the
// results concatenate into one set without aliasing. The second return value is
// how many of them came from the Manager window, which is what tells a program
// that actually built a VM from one that was rejected before the Manager did
// any work, and the third which start stages it reached.
func (runner *Runner) xhDrainAll() ([]uint64, int, [len(xhWaypoints)]bool) {
	var pcs []uint64
	if runner.xh != nil {
		pcs = runner.xh.drain()
	}
	mgr := 0
	var reached [len(xhWaypoints)]bool
	if runner.xhMgr != nil {
		m := runner.xhMgr.drain()
		mgr = len(m)
		for _, pc := range m {
			for i, r := range runner.xhWayRanges {
				if r.lo != 0 && pc >= r.lo && pc < r.hi {
					reached[i] = true
				}
			}
		}
		pcs = append(pcs, m...)
	}
	return pcs, mgr, reached
}

// xhNoteProgram records whether a program drove the Manager at all, and logs
// the running ratio.
//
// Why this exists: a document the Manager rejects early -- one carrying an
// attribute that is invalid for an untrusted configuration, say -- spends a
// whole program and builds no VM, yet nothing in the fuzzer's own numbers shows
// it. The symptom is only that coverage rises slowly, which is indistinguishable
// from honest saturation. Two such defects (crash-fatal, and a vdevice skeleton
// the parser rejected) were each found by someone reading Manager source, not by
// this side noticing. Counting the programs that reached the Manager at all
// makes that waste visible directly instead of inferring it from how bursty the
// coverage curve is.
// xhWaypoints are ordered stages of secondary-VM start, each named by one
// function in the manager's ELF. A program's drained manager window is matched
// against them to record HOW FAR it got.
//
// Why ordered waypoints rather than a single "did it start" flag: most programs
// reach the manager and then fail somewhere before power-on, and a block count
// cannot say where -- a configuration refused at the very first stage still
// parses the whole document and covers thousands of blocks, so the failures all
// look alike. Knowing which stage they stop at is what turns "most programs
// waste themselves" into something that can be acted on.
var xhWaypoints = [...]struct {
	name string
	sym  string
}{
	// "reached the manager" is too weak on its own: any /dev/gunyah operation
	// makes the manager do RPC work, with no configuration document involved.
	// This one runs only when a guest configuration is actually submitted, so
	// it separates "never tried to build a VM" from "tried and was refused".
	{"submitted", "read_secondary_configuration"},
	{"configured", "realize_configuration"},
	{"objects", "realize_vm_objects_with_watchdog"},
	{"projected", "install_secondary_projection_transaction"},
	{"started", "power_on_vm"},
}

type xhRange struct{ lo, hi uint64 }

// xhResolveWaypoints locates each waypoint in the manager's ELF, rebased to
// where the hypervisor loads it. A waypoint that cannot be found is left zero
// and never matches, so a renamed function degrades to "not reached" rather
// than to a wrong answer.
func xhResolveWaypoints(objPath string, loadAddr uint64) []xhRange {
	out := make([]xhRange, len(xhWaypoints))
	if objPath == "" || loadAddr == 0 {
		return out
	}
	f, err := elf.Open(objPath)
	if err != nil {
		return out
	}
	defer f.Close()
	syms, err := f.Symbols()
	if err != nil {
		return out
	}
	for i, wp := range xhWaypoints {
		// Symbols are mangled monomorphisations, so the match is on a
		// substring -- but taking the first hit and stopping would silently
		// pick whichever symbol the table happens to list first, and that
		// order is not stable across links. One of these substrings does
		// appear in two symbols (a monomorphisation carrying the other's
		// closure in its name), so a reordering would move the waypoint onto
		// an unrelated function and report a confident WRONG answer, which is
		// worse than reporting nothing. Require exactly one match.
		var hits []elf.Symbol
		for _, sym := range syms {
			if elf.ST_TYPE(sym.Info) != elf.STT_FUNC || sym.Size == 0 {
				continue
			}
			if strings.Contains(sym.Name, wp.sym) {
				hits = append(hits, sym)
			}
		}
		switch len(hits) {
		case 1:
			out[i] = xhRange{loadAddr + hits[0].Value, loadAddr + hits[0].Value + hits[0].Size}
		case 0:
			log.Logf(0, "xhyper: waypoint %v (%v) not found; it will read as never reached",
				wp.name, wp.sym)
		default:
			log.Logf(0, "xhyper: waypoint %v (%v) matches %v symbols, so it is disabled "+
				"rather than guessed; narrow the substring", wp.name, wp.sym, len(hits))
		}
	}
	return out
}

// xhNoteOutcomes records whether the calls that build and start a VM actually
// succeeded, taken from the program's own errno.
//
// Why this is needed on top of the waypoints: every waypoint fires on ENTERING
// a function, so "reached power-on" is not "powered on" -- that function can
// still fail on a missing boot vCPU or on the entry address the document gave.
// The start call's errno covers the whole operation including its last step, so
// it is the one signal that says succeeded, while the waypoints only say where
// a failure happened. The two answer different questions and neither replaces
// the other. This information was available from the first day; it took looking
// outside the coverage data to notice.
func (runner *Runner) xhNoteOutcomes(prog *prog.Prog, calls []*flatrpc.CallInfo) {
	// Per PROGRAM, not per call. Most corpus programs configure the same VM
	// twice, and the second attempt necessarily fails because the VM is
	// already configured -- the descriptions let the pseudo-call take a VM fd
	// without consuming it, and coverage guidance likes the shape because the
	// failure path is new coverage. Counting calls therefore produced exactly
	// as many failures as successes, a number too tidy to be a property of the
	// target. What matters is whether a program managed it at all.
	var setupTried, setupOK, startTried, startOK bool
	var startErrno int32
	for i, call := range prog.Calls {
		if i >= len(calls) || calls[i] == nil {
			break
		}
		ok := calls[i].Error == 0
		switch call.Meta.CallName {
		case "syz_gunyah_setup_vm", "syz_gunyah_setup_vm_lend":
			setupTried = true
			setupOK = setupOK || ok
		case "syz_gunyah_add_vcpu":
			// This helper issues the start itself, so its result is the
			// start's result.
			startTried = true
			startOK = startOK || ok
			if !ok && startErrno == 0 {
				startErrno = calls[i].Error
			}
		}
	}
	if setupTried {
		if setupOK {
			runner.xhSetupOK++
		} else {
			runner.xhSetupFail++
		}
	}
	if startTried {
		if startOK {
			runner.xhStartOK++
		} else {
			runner.xhStartFail++
			if runner.xhStartErrno == nil {
				runner.xhStartErrno = make(map[int32]int)
			}
			runner.xhStartErrno[startErrno]++
		}
	}
}

// xhMgrBuckets splits programs by how much Manager code they drove. The
// boundaries come from measurement, not from round numbers: on this image a VM
// that starts covers roughly 6800 Manager blocks and one the Manager rejects
// after parsing the whole document still covers 3100-3800, so those two
// outcomes land in different buckets. A mean alone cannot tell them apart --
// it is the SHAPE that says whether there is a third population.
var xhMgrBuckets = [...]int{1, 100, 1000, 3000, 6000}

func (runner *Runner) xhNoteProgram(mgrBlocks int, reached [len(xhWaypoints)]bool) {
	runner.xhProgs++
	if mgrBlocks == 0 {
		runner.xhProgsNoMgr++
	}
	var path uint32
	for i, hit := range reached {
		if hit {
			runner.xhWayHits[i]++
			path |= 1 << uint(i)
		}
	}
	if runner.xhWayPaths == nil {
		runner.xhWayPaths = make(map[uint32]int)
	}
	runner.xhWayPaths[path]++
	if reached[len(reached)-1] {
		runner.xhStarted++
	}
	b := 0
	for b < len(xhMgrBuckets) && mgrBlocks >= xhMgrBuckets[b] {
		b++
	}
	runner.xhMgrHist[b]++
	const every = 50
	if runner.xhProgs%every != 0 {
		return
	}
	drove := runner.xhProgs - runner.xhProgsNoMgr
	log.Logf(0, "xhyper: %v/%v drove the Manager (%.1f%%), %v STARTED a VM (%.1f%%); "+
		"blocks per program 0:%v 1-99:%v 100-999:%v 1k-3k:%v 3k-6k:%v 6k+:%v",
		drove, runner.xhProgs, 100*float64(drove)/float64(runner.xhProgs),
		runner.xhStarted, 100*float64(runner.xhStarted)/float64(runner.xhProgs),
		runner.xhMgrHist[0], runner.xhMgrHist[1], runner.xhMgrHist[2],
		runner.xhMgrHist[3], runner.xhMgrHist[4], runner.xhMgrHist[5])
	var stages string
	for i, wp := range xhWaypoints {
		stages += fmt.Sprintf(" %v:%v", wp.name, runner.xhWayHits[i])
	}
	log.Logf(0, "xhyper: start stages reached out of %v programs:%v", runner.xhProgs, stages)
	paths := make([]uint32, 0, len(runner.xhWayPaths))
	for k := range runner.xhWayPaths {
		paths = append(paths, k)
	}
	sort.Slice(paths, func(a, b int) bool {
		return runner.xhWayPaths[paths[a]] > runner.xhWayPaths[paths[b]]
	})
	var out string
	for n, k := range paths {
		if n == 6 {
			break
		}
		label := "none"
		if k != 0 {
			label = ""
			for i, wp := range xhWaypoints {
				if k&(1<<uint(i)) != 0 {
					if label != "" {
						label += "+"
					}
					label += wp.name
				}
			}
		}
		out += fmt.Sprintf(" [%v]=%v", label, runner.xhWayPaths[k])
	}
	log.Logf(0, "xhyper: stage paths taken:%v", out)
	// "setup ioctls", not "configured". The configuration ioctls do NOT reach the
	// Manager: GUNYAH_VM_SET_DTB_CONFIG and its siblings only store the struct in
	// the host driver and return 0 (vm_mgr.c:876-890); every gunyah_rm_* call --
	// alloc_vmid, set_boot_context, set_demand_paging -- is on the START path.
	// Calling this counter "configured" put the same word on two different things,
	// because the waypoint below named "configured" really is the Manager running
	// realize_configuration. Reading 112 here against 21 there then looks like a
	// contradiction and invites an explanation for a phenomenon that is not there.
	log.Logf(0, "xhyper: per-program outcomes: setup ioctls ok=%v fail=%v, started ok=%v fail=%v",
		runner.xhSetupOK, runner.xhSetupFail, runner.xhStartOK, runner.xhStartFail)
	if len(runner.xhStartErrno) != 0 {
		errnos := make([]int32, 0, len(runner.xhStartErrno))
		for k := range runner.xhStartErrno {
			errnos = append(errnos, k)
		}
		sort.Slice(errnos, func(a, b int) bool {
			return runner.xhStartErrno[errnos[a]] > runner.xhStartErrno[errnos[b]]
		})
		var errOut string
		for n, k := range errnos {
			if n == 6 {
				break
			}
			errOut += fmt.Sprintf(" errno%v=%v", k, runner.xhStartErrno[k])
		}
		log.Logf(0, "xhyper: start failures by errno:%v", errOut)
	}
}

func (runner *Runner) ConnectionLoop() error {
	if runner.updInfo != nil {
		runner.updInfo(func(info *RunnerInfo) {
			info.Status = "executing"
		})
	}

	runner.mu.Lock()
	stopped := runner.stopped
	if !stopped {
		runner.finished = make(chan bool)
	}
	runner.mu.Unlock()

	if stopped {
		// The instance was shut down in between, see the shutdown code.
		return nil
	}
	defer close(runner.finished)

	// The executor has connected and the VM is up, but nothing has been
	// dispatched yet, so the window right now holds exactly what booting
	// executed. Take that as this VM lifecycle's boot mask.
	//
	// It has to be taken here and not from the first exec result: by the time
	// a result arrives its program has already run, so that window would be
	// "boot + first program", and the first program's own blocks would then be
	// subtracted for the rest of the VM's life -- an arbitrary hole whose
	// shape depends on whichever program happened to run first, and a very
	// hard one to guess at later when asking why some function is never
	// covered.
	if runner.xh != nil || runner.xhMgr != nil {
		pcs, _, _ := runner.xhDrainAll()
		runner.xhBootMask = make(map[uint64]bool, len(pcs))
		for _, pc := range pcs {
			runner.xhBootMask[pc] = true
		}
	}

	var infoc chan []byte
	defer func() {
		if infoc != nil {
			infoc <- []byte("VM has crashed")
		}
	}()

	runner.lastRequestTime = time.Now()
	for {
		if infoc == nil {
			select {
			case infoc = <-runner.infoc:
				err := runner.sendStateRequest()
				if err != nil {
					return err
				}
			default:
			}
		}
		for len(runner.requests)-len(runner.executing) < 2*runner.procs {
			req := runner.source.Next(runner.id)
			if req == nil {
				break
			}
			if err := runner.sendRequest(req); err != nil {
				return err
			}
			runner.lastRequestTime = time.Now()
		}
		if len(runner.requests) == 0 {
			if !runner.Alive() {
				return nil
			}
			if err := runner.handleIdle(); err != nil {
				return err
			}
			continue
		}
		raw, err := wrappedRecv[*flatrpc.ExecutorMessageRaw](runner)
		if err != nil {
			return err
		}
		if raw.Msg == nil || raw.Msg.Value == nil {
			return errors.New("received no message")
		}
		switch msg := raw.Msg.Value.(type) {
		case *flatrpc.ExecutingMessage:
			err = runner.handleExecutingMessage(msg)
		case *flatrpc.ExecResult:
			err = runner.handleExecResult(msg)
		case *flatrpc.StateResult:
			buf := new(bytes.Buffer)
			fmt.Fprintf(buf, "pending requests on the VM:")
			for id := range runner.requests {
				fmt.Fprintf(buf, " %v", id)
			}
			fmt.Fprintf(buf, "\n\n")
			result := append(buf.Bytes(), msg.Data...)
			if infoc != nil {
				infoc <- result
				infoc = nil
			} else {
				// The request was solicited in detectTimeout().
				log.Logf(0, "status result: %s", result)
			}
		default:
			return fmt.Errorf("received unknown message type %T", msg)
		}
		if err != nil {
			return err
		}
	}
}

// handleIdle is called when the VM has no pending requests.
// It sends a dummy keepalive request if the queue has been empty for a while.
// This proves both the OS and the C++ executor's fork server are still responsive,
// and importantly triggers the ExecutingMessage to update lastExecuteTime in the VM monitor.
func (runner *Runner) handleIdle() error {
	if time.Since(runner.lastRequestTime) > 10*time.Second {
		dummyReq := &queue.Request{
			Type: flatrpc.RequestTypeProgram,
			Prog: &prog.Prog{
				Target: runner.progTarget,
				Calls:  []*prog.Call{},
			},
			ExecOpts: flatrpc.ExecOpts{
				EnvFlags: flatrpc.ExecEnvSandboxNone,
			},
		}
		if err := runner.sendRequest(dummyReq); err != nil {
			return err
		}
		runner.lastRequestTime = time.Now()
		return nil
	}

	// The runner has no new requests, so don't wait to receive anything from it.
	time.Sleep(10 * time.Millisecond)
	return nil
}

func wrappedRecv[Raw flatrpc.RecvType[T], T any](runner *Runner) (*T, error) {
	if runner.debugTimeouts {
		abort := runner.detectTimeout()
		defer close(abort)
	}
	return flatrpc.Recv[Raw](runner.conn)
}

func (runner *Runner) detectTimeout() chan struct{} {
	abort := make(chan struct{})
	go func() {
		select {
		case <-time.After(time.Minute):
			log.Logf(0, "timed out waiting for executor reply, aborting the connection in 1 minute")
			go func() {
				time.Sleep(time.Minute)
				runner.conn.Close()
			}()
			err := runner.sendStateRequest()
			if err != nil {
				log.Logf(0, "failed to send state request: %v", err)
				return
			}

		case <-abort:
			return
		case <-runner.finished:
			return
		}
	}()
	return abort
}

func (runner *Runner) sendStateRequest() error {
	msg := &flatrpc.HostMessage{
		Msg: &flatrpc.HostMessages{
			Type:  flatrpc.HostMessagesRawStateRequest,
			Value: &flatrpc.StateRequest{},
		},
	}
	return flatrpc.Send(runner.conn, msg)
}

func (runner *Runner) sendRequest(req *queue.Request) error {
	if err := req.Validate(); err != nil {
		panic(err)
	}
	runner.nextRequestID++
	id := runner.nextRequestID
	var flags flatrpc.RequestFlag
	if req.ReturnOutput {
		flags |= flatrpc.RequestFlagReturnOutput
	}
	if req.ReturnError {
		flags |= flatrpc.RequestFlagReturnError
	}
	allSignal := make([]int32, len(req.ReturnAllSignal))
	for i, call := range req.ReturnAllSignal {
		allSignal[i] = int32(call)
	}
	opts := req.ExecOpts
	if runner.debug {
		opts.EnvFlags |= flatrpc.ExecEnvDebug
	}
	var data []byte
	switch req.Type {
	case flatrpc.RequestTypeProgram:
		progData, err := req.Prog.SerializeForExec()
		if err != nil {
			// It's bad if we systematically fail to serialize programs,
			// but so far we don't have a better handling than counting this.
			// This error is observed a lot on the seeded syz_mount_image calls.
			runner.stats.statExecBufferTooSmall.Add(1)
			req.Done(&queue.Result{
				Status: queue.ExecFailure,
				Err:    fmt.Errorf("program serialization failed: %w", err),
			})
			return nil
		}
		data = progData
	case flatrpc.RequestTypeBinary:
		fileData, err := os.ReadFile(req.BinaryFile)
		if err != nil {
			req.Done(&queue.Result{
				Status: queue.ExecFailure,
				Err:    err,
			})
			return nil
		}
		data = fileData
	case flatrpc.RequestTypeGlob:
		data = append([]byte(req.GlobPattern), 0)
		flags |= flatrpc.RequestFlagReturnOutput
	default:
		panic("unhandled request type")
	}
	var avoid uint64
	for _, id := range req.Avoid {
		if id.VM == runner.id {
			avoid |= uint64(1 << id.Proc)
		}
	}
	msg := &flatrpc.HostMessage{
		Msg: &flatrpc.HostMessages{
			Type: flatrpc.HostMessagesRawExecRequest,
			Value: &flatrpc.ExecRequest{
				Id:        id,
				Type:      req.Type,
				Avoid:     avoid,
				Data:      data,
				Flags:     flags,
				ExecOpts:  &opts,
				AllSignal: allSignal,
			},
		},
	}
	runner.requests[id] = req
	return flatrpc.Send(runner.conn, msg)
}

func (runner *Runner) handleExecutingMessage(msg *flatrpc.ExecutingMessage) error {
	req := runner.requests[msg.Id]
	if req == nil {
		if runner.hanged[msg.Id] {
			return nil
		}
		return fmt.Errorf("can't find executing request %v", msg.Id)
	}
	proc := int(msg.ProcId)
	if proc < 0 || proc >= prog.MaxPids {
		return fmt.Errorf("got bad proc id %v", proc)
	}
	runner.stats.statExecs.Add(1)
	if msg.Try == 0 {
		if msg.WaitDuration != 0 {
			runner.stats.statNoExecRequests.Add(1)
			// Cap wait duration to 1 second to avoid extreme peaks on the graph
			// which make it impossible to see real data (the rest becomes a flat line).
			runner.stats.statNoExecDuration.Add(int(min(msg.WaitDuration, 1e9)))
		}
	} else {
		runner.stats.statExecRetries.Add(1)
	}
	var data []byte
	switch req.Type {
	case flatrpc.RequestTypeProgram:
		data = req.Prog.Serialize()
	case flatrpc.RequestTypeBinary:
		data = []byte(fmt.Sprintf("executing binary %v\n", req.BinaryFile))
	case flatrpc.RequestTypeGlob:
		data = []byte(fmt.Sprintf("expanding glob: %v\n", req.GlobPattern))
	default:
		panic(fmt.Sprintf("unhandled request type %v", req.Type))
	}
	runner.lastExec.Note(int(msg.Id), proc, data, osutil.MonotonicNano())
	select {
	case runner.injectExec <- true:
	default:
	}
	runner.executing[msg.Id] = true
	return nil
}

func (runner *Runner) handleExecResult(msg *flatrpc.ExecResult) error {
	req := runner.requests[msg.Id]
	if req == nil {
		if runner.hanged[msg.Id] {
			// Got result for a program that was previously reported hanged
			// (probably execution was just extremely slow). Can't report result
			// to pkg/fuzzer since it already handled completion of the request,
			// but shouldn't report an error and crash the VM as well.
			delete(runner.hanged, msg.Id)
			return nil
		}
		return fmt.Errorf("can't find executed request %v", msg.Id)
	}
	delete(runner.requests, msg.Id)
	delete(runner.executing, msg.Id)
	if req.Type == flatrpc.RequestTypeProgram && msg.Info != nil {
		for len(msg.Info.Calls) < len(req.Prog.Calls) {
			msg.Info.Calls = append(msg.Info.Calls, &flatrpc.CallInfo{
				Error: 999,
			})
		}
		msg.Info.Calls = msg.Info.Calls[:len(req.Prog.Calls)]
		if msg.Info.Freshness == 0 {
			runner.stats.statExecutorRestarts.Add(1)
		}
		for _, call := range msg.Info.Calls {
			runner.convertCallInfo(call)
		}
		if len(msg.Info.ExtraRaw) != 0 {
			msg.Info.Extra = msg.Info.ExtraRaw[0]
			for _, info := range msg.Info.ExtraRaw[1:] {
				// All processing in the fuzzer later will convert signal/cover to maps and dedup,
				// so there is little point in deduping here.
				msg.Info.Extra.Cover = append(msg.Info.Extra.Cover, info.Cover...)
				msg.Info.Extra.Signal = append(msg.Info.Extra.Signal, info.Signal...)
			}
			msg.Info.ExtraRaw = nil
			runner.convertCallInfo(msg.Info.Extra)
		}
		// EL2 PCs are not guest-kernel text. Append them after convertCallInfo,
		// which drops addresses outside the kernel module map.
		if runner.xh != nil || runner.xhMgr != nil {
			pcs, mgrBlocks, reached := runner.xhDrainAll()
			if msg.Info.Freshness != 0 {
				runner.xhNoteOutcomes(req.Prog, msg.Info.Calls)
			}
			// Freshness 0 means the executor (re)started just before this
			// program, so the drained window also holds the VM's boot and
			// background hypervisor coverage.
			//
			// Dropping that window is not enough on its own. Those blocks then
			// never enter the fuzzer's known signal, so the first LATER program
			// that happens to re-walk boot code looks like it discovered all of
			// them at once: it is judged hugely valuable, enters the corpus, and
			// -- because mutation budget follows signal volume -- keeps most of
			// the mutation budget while having done nothing special. That is
			// exactly how one ordinary program came to hold 57% of the signal.
			//
			// So keep the first window as this VM lifecycle's boot mask and
			// subtract it from every later window's SIGNAL: re-walking what
			// booting already walked is then worth nothing, equally for every
			// program. Coverage is left untouched, because the coverage report
			// is for humans and must stay faithful. Only the first Freshness 0
			// builds the mask; a later executor restart inside the same VM
			// drains its window (to clear it and re-attach) without replacing
			// the mask, which describes the VM, not the executor.
			if msg.Info.Freshness == 0 {
				// Executor restart: this window is background activity, not
				// this program's. Drop it, as before. The boot mask is built
				// in ConnectionLoop and is not touched here.
			} else if runner.xhNoteProgram(mgrBlocks, reached); len(pcs) != 0 {
				if msg.Info.Extra == nil {
					msg.Info.Extra = &flatrpc.CallInfo{}
				}
				msg.Info.Extra.Cover = append(msg.Info.Extra.Cover, pcs...)
				sig := pcs
				if len(runner.xhBootMask) != 0 {
					sig = make([]uint64, 0, len(pcs))
					for _, pc := range pcs {
						if !runner.xhBootMask[pc] {
							sig = append(sig, pc)
						}
					}
				}
				if len(sig) != 0 {
					msg.Info.Extra.Signal = append(msg.Info.Extra.Signal, sig...)
				}
			}
		}
		if !runner.cover && req.ExecOpts.ExecFlags&flatrpc.ExecFlagCollectSignal != 0 {
			// Coverage collection is disabled, but signal was requested => use a substitute signal.
			// Note that we do it after all the processing above in order to prevent it from being
			// filtered out.
			addFallbackSignal(req.Prog, msg.Info)
		}
	}
	status := queue.Success
	var resErr error
	if msg.Error != "" {
		status = queue.ExecFailure
		resErr = errors.New(msg.Error)
	} else if msg.Hanged {
		status = queue.Hanged
		if req.Type == flatrpc.RequestTypeProgram {
			// We only track the latest executed programs.
			runner.lastExec.Hanged(int(msg.Id), int(msg.Proc), req.Prog.Serialize(), osutil.MonotonicNano())
		}
		runner.hanged[msg.Id] = true
	}
	req.Done(&queue.Result{
		Executor: queue.ExecutorID{
			VM:   runner.id,
			Proc: int(msg.Proc),
		},
		Status: status,
		Info:   msg.Info,
		Output: slices.Clone(msg.Output),
		Err:    resErr,
	})
	return nil
}

func (runner *Runner) convertCallInfo(call *flatrpc.CallInfo) {
	call.Cover = runner.canonicalizer.Canonicalize(call.Cover)
	call.Signal = runner.canonicalizer.Canonicalize(call.Signal)

	call.Comps = slices.DeleteFunc(call.Comps, func(cmp *flatrpc.Comparison) bool {
		converted := runner.canonicalizer.Canonicalize([]uint64{cmp.Pc})
		if len(converted) == 0 {
			return true
		}
		cmp.Pc = converted[0]
		return false
	})

	// Check signal belongs to kernel addresses.
	// Mismatching addresses can mean either corrupted VM memory, or that the fuzzer somehow
	// managed to inject output signal. If we see any bogus signal, drop whole signal
	// (we don't want programs that can inject bogus coverage to end up in the corpus).
	var kernelAddresses targets.KernelAddresses
	if runner.filterSignal {
		kernelAddresses = runner.sysTarget.KernelAddresses
	}
	textStart, textEnd := kernelAddresses.TextStart, kernelAddresses.TextEnd
	if textStart != 0 {
		for _, sig := range call.Signal {
			if sig < textStart || sig > textEnd {
				call.Signal = []uint64{}
				call.Cover = []uint64{}
				break
			}
		}
	}

	// Filter out kernel physical memory addresses.
	// These are internal kernel comparisons and should not be interesting.
	dataStart, dataEnd := kernelAddresses.DataStart, kernelAddresses.DataEnd
	if len(call.Comps) != 0 && (textStart != 0 || dataStart != 0) {
		if runner.sysTarget.PtrSize == 4 {
			// These will appear sign-extended in comparison operands.
			textStart = uint64(int64(int32(textStart)))
			textEnd = uint64(int64(int32(textEnd)))
			dataStart = uint64(int64(int32(dataStart)))
			dataEnd = uint64(int64(int32(dataEnd)))
		}
		isKptr := func(val uint64) bool {
			return val >= textStart && val <= textEnd || val >= dataStart && val <= dataEnd || val == 0
		}
		call.Comps = slices.DeleteFunc(call.Comps, func(cmp *flatrpc.Comparison) bool {
			return isKptr(cmp.Op1) && isKptr(cmp.Op2)
		})
	}
}

func (runner *Runner) SendSignalUpdate(plus []uint64) error {
	msg := &flatrpc.HostMessage{
		Msg: &flatrpc.HostMessages{
			Type: flatrpc.HostMessagesRawSignalUpdate,
			Value: &flatrpc.SignalUpdate{
				NewMax: runner.canonicalizer.Decanonicalize(plus),
			},
		},
	}
	return flatrpc.Send(runner.conn, msg)
}

func (runner *Runner) SendCorpusTriaged() error {
	msg := &flatrpc.HostMessage{
		Msg: &flatrpc.HostMessages{
			Type:  flatrpc.HostMessagesRawCorpusTriaged,
			Value: &flatrpc.CorpusTriaged{},
		},
	}
	return flatrpc.Send(runner.conn, msg)
}

func (runner *Runner) Stop() {
	runner.mu.Lock()
	runner.stopped = true
	conn := runner.conn
	runner.mu.Unlock()
	if conn != nil {
		conn.Close()
	}
}

func (runner *Runner) Shutdown(crashed bool, extraExecs ...report.ExecutorInfo) []ExecRecord {
	runner.mu.Lock()
	runner.stopped = true
	finished := runner.finished
	runner.mu.Unlock()

	if finished != nil {
		// Wait for the connection goroutine to finish and stop touching data.
		<-finished
	}
	if runner.xh != nil {
		runner.xh.close()
	}
	if runner.xhMgr != nil {
		runner.xhMgr.close()
	}
	records := runner.lastExec.Collect()
	for _, info := range extraExecs {
		req := runner.requests[int64(info.ExecID)]
		// If the request is in executing, it's also already in the records slice.
		if req != nil && !runner.executing[int64(info.ExecID)] {
			records = append(records, ExecRecord{
				ID:   info.ExecID,
				Proc: info.ProcID,
				Prog: req.Prog.Serialize(),
			})
		}
	}
	for id, req := range runner.requests {
		status := queue.Restarted
		if crashed && runner.executing[id] {
			status = queue.Crashed
		}
		req.Done(&queue.Result{Status: status})
	}
	return records
}

func (runner *Runner) MachineInfo() []byte {
	runner.mu.Lock()
	defer runner.mu.Unlock()
	return runner.machineInfo
}

func (runner *Runner) QueryStatus() []byte {
	resc := make(chan []byte, 1)
	timeout := time.After(time.Minute)
	select {
	case runner.infoc <- resc:
	case <-timeout:
		return []byte("VM loop is not responding")
	}
	select {
	case res := <-resc:
		return res
	case <-timeout:
		return []byte("VM is not responding")
	}
}

func (runner *Runner) Alive() bool {
	runner.mu.Lock()
	defer runner.mu.Unlock()
	return runner.conn != nil && !runner.stopped
}

// addFallbackSignal computes simple fallback signal in cases we don't have real coverage signal.
// We use syscall number or-ed with returned errno value as signal.
// At least this gives us all combinations of syscall+errno.
func addFallbackSignal(p *prog.Prog, info *flatrpc.ProgInfo) {
	callInfos := make([]prog.CallInfo, len(info.Calls))
	for i, inf := range info.Calls {
		if inf.Flags&flatrpc.CallFlagExecuted != 0 {
			callInfos[i].Flags |= prog.CallExecuted
		}
		if inf.Flags&flatrpc.CallFlagFinished != 0 {
			callInfos[i].Flags |= prog.CallFinished
		}
		if inf.Flags&flatrpc.CallFlagBlocked != 0 {
			callInfos[i].Flags |= prog.CallBlocked
		}
		callInfos[i].Errno = int(inf.Error)
	}
	p.FallbackSignal(callInfos)
	for i, inf := range callInfos {
		info.Calls[i].Signal = inf.Signal
	}
}
