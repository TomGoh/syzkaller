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
	// Resolved once per runner from the manager ELF; zero when not configured.
	xhPowerOnLo uint64
	xhPowerOnHi uint64

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
// any work, and the third whether it got as far as powering a VM on.
func (runner *Runner) xhDrainAll() ([]uint64, int, bool) {
	var pcs []uint64
	if runner.xh != nil {
		pcs = runner.xh.drain()
	}
	mgr, poweredOn := 0, false
	if runner.xhMgr != nil {
		m := runner.xhMgr.drain()
		mgr = len(m)
		for _, pc := range m {
			if runner.xhPowerOnLo != 0 && pc >= runner.xhPowerOnLo && pc < runner.xhPowerOnHi {
				poweredOn = true
				break
			}
		}
		pcs = append(pcs, m...)
	}
	return pcs, mgr, poweredOn
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
// xhPowerOnRange finds ConfigurationDomain::power_on_vm in the manager's ELF,
// rebased to where the hypervisor loads it.
//
// Why that function: it is the last thing start_vm does, after projection,
// memory policy and the virtual-RTC rollback have all succeeded, and its body
// writes the boot vCPU's registers. So a program whose drained manager window
// touches it demonstrably STARTED a VM, while one that was refused earlier did
// not -- and the two are otherwise hard to tell apart, because a refused
// configuration still parses the whole document and so still covers thousands
// of manager blocks. Counting blocks alone cannot separate them; this can.
func xhPowerOnRange(objPath string, loadAddr uint64) (lo, hi uint64) {
	if objPath == "" || loadAddr == 0 {
		return 0, 0
	}
	f, err := elf.Open(objPath)
	if err != nil {
		return 0, 0
	}
	defer f.Close()
	syms, err := f.Symbols()
	if err != nil {
		return 0, 0
	}
	for _, s := range syms {
		if elf.ST_TYPE(s.Info) != elf.STT_FUNC || s.Size == 0 {
			continue
		}
		// The symbol is a mangled monomorphisation, so match on the substring
		// rather than on an exact name.
		if strings.Contains(s.Name, "power_on_vm") {
			return loadAddr + s.Value, loadAddr + s.Value + s.Size
		}
	}
	return 0, 0
}

// xhMgrBuckets splits programs by how much Manager code they drove. The
// boundaries come from measurement, not from round numbers: on this image a VM
// that starts covers roughly 6800 Manager blocks and one the Manager rejects
// after parsing the whole document still covers 3100-3800, so those two
// outcomes land in different buckets. A mean alone cannot tell them apart --
// it is the SHAPE that says whether there is a third population.
var xhMgrBuckets = [...]int{1, 100, 1000, 3000, 6000}

func (runner *Runner) xhNoteProgram(mgrBlocks int, poweredOn bool) {
	runner.xhProgs++
	if mgrBlocks == 0 {
		runner.xhProgsNoMgr++
	}
	if poweredOn {
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
			pcs, mgrBlocks, poweredOn := runner.xhDrainAll()
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
			} else if runner.xhNoteProgram(mgrBlocks, poweredOn); len(pcs) != 0 {
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
