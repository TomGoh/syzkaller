// Copyright 2024 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package fuzzer

import (
	"context"
	"fmt"
	"testing"

	"github.com/google/syzkaller/pkg/corpus"
	"github.com/google/syzkaller/pkg/cover"
	"github.com/google/syzkaller/pkg/flatrpc"
	"github.com/google/syzkaller/pkg/fuzzer/queue"
	"github.com/google/syzkaller/pkg/signal"
	"github.com/google/syzkaller/prog"
	"github.com/google/syzkaller/sys/targets"
	"github.com/stretchr/testify/assert"
)

func TestDeflake(t *testing.T) {
	type Test struct {
		Info triageCall
		Exec func(run uint64) (errno int32, signal []uint64, cover []uint64)
		Runs uint64
	}
	tests := []Test{
		{
			Info: triageCall{
				newSignal: signal.FromRaw([]uint64{0, 1, 2, 3, 4}, 0),
				cover:     cover.FromRaw([]uint64{10, 20}),
			},
			Exec: func(run uint64) (int32, []uint64, []uint64) {
				// For first, we return 1. For second, 2. And so on.
				return 0, []uint64{run}, []uint64{10, 20}
			},
			Runs: 3,
		},
		{
			Info: triageCall{
				newSignal: signal.FromRaw([]uint64{0, 1, 2}, 0),
				// Cover is a union of all coverages.
				cover: cover.FromRaw([]uint64{10, 20, 30, 40, 100}),
				// 0, 2, 6 were in three resuls.
				stableSignal: signal.FromRaw([]uint64{0, 2, 6}, 0),
				// 0, 2 were also in newSignal.
				newStableSignal: signal.FromRaw([]uint64{0, 2, 6}, 0),
			},
			Exec: func(run uint64) (int32, []uint64, []uint64) {
				switch run {
				case 1:
					return 0, []uint64{0, 2, 4, 6, 8}, []uint64{10, 20}
				case 2:
					// This one should be ignored -- it has a different errno.
					return 1, []uint64{0, 1, 2}, []uint64{100}
				case 3:
					return 0, []uint64{0, 2, 4, 6, 8}, []uint64{20, 30}
				case 4:
					return 0, []uint64{0, 2, 6}, []uint64{30, 40}
				}
				panic("unrechable")
			},
			Runs: 4,
		},
		{
			Info: triageCall{
				newSignal:       signal.FromRaw([]uint64{0, 1, 2, 3, 4}, 3),
				cover:           cover.FromRaw([]uint64{10, 20}),
				stableSignal:    signal.FromRaw([]uint64{2}, 0),
				newStableSignal: signal.FromRaw([]uint64{2}, 0),
			},
			Exec: func(run uint64) (int32, []uint64, []uint64) {
				// For first, we return 0 and 1. For second, 1 and 2. And so on.
				return 0, []uint64{run, run + 1}, []uint64{10, 20}
			},
			Runs: 2,
		},
	}

	target, err := prog.GetTarget(targets.TestOS, targets.TestArch64Fuzz)
	assert.NoError(t, err)
	const anyTestProg = `syz_compare(&AUTO="00000000", 0x4, &AUTO=@conditional={0x0, @void, @void, @void}, AUTO)`
	prog, err := target.Deserialize([]byte(anyTestProg), prog.NonStrict)
	assert.NoError(t, err)

	for i, test := range tests {
		t.Run(fmt.Sprint(i), func(t *testing.T) {
			info := test.Info
			info.signals[0] = test.Info.newSignal.Copy()
			info.cover = nil
			info.stableSignal = nil
			info.newStableSignal = nil
			testJob := &triageJob{
				p:     prog,
				calls: map[int]*triageCall{0: &info},
				fuzzer: &Fuzzer{
					Cover:  newCover(),
					Config: &Config{},
				},
				info: &JobInfo{},
			}

			var run uint64
			stop := testJob.deflake(func(_ *queue.Request, _ ProgFlags) *queue.Result {
				run++
				errno, signal, cover := test.Exec(run)
				return &queue.Result{
					Info: &flatrpc.ProgInfo{
						Calls: []*flatrpc.CallInfo{{
							Error:  errno,
							Signal: signal,
							Cover:  cover,
						}},
					},
				}
			})

			assert.False(t, stop)
			assert.Equal(t, run, test.Runs)
			assert.ElementsMatch(t, info.cover.Serialize(), test.Info.cover.Serialize())
			assert.ElementsMatch(t, info.stableSignal.ToRaw(), test.Info.stableSignal.ToRaw())
			assert.ElementsMatch(t, info.newStableSignal.ToRaw(), test.Info.newStableSignal.ToRaw())
		})
	}
}

// Disjoint per-run extra signal is the XHyper drain case: each triage
// re-exec covers a different set of EL2 blocks, so the deflake intersection
// is empty. With XHyperCover the new max-signal is kept as stable.
func TestDeflakeXHyperExtraStable(t *testing.T) {
	target, err := prog.GetTarget(targets.TestOS, targets.TestArch64Fuzz)
	assert.NoError(t, err)
	const anyTestProg = `syz_compare(&AUTO="00000000", 0x4, &AUTO=@conditional={0x0, @void, @void, @void}, AUTO)`
	p, err := target.Deserialize([]byte(anyTestProg), prog.NonStrict)
	assert.NoError(t, err)

	runDeflake := func(xhyper bool) (uint64, *triageCall) {
		var runs uint64
		info := &triageCall{
			newSignal: signal.FromRaw([]uint64{1}, 0),
		}
		info.signals[0] = info.newSignal.Copy()
		job := &triageJob{
			p:     p,
			calls: map[int]*triageCall{-1: info},
			fuzzer: &Fuzzer{
				Cover:  newCover(),
				Config: &Config{XHyperCover: xhyper},
			},
			info: &JobInfo{},
		}
		stop := job.deflake(func(_ *queue.Request, _ ProgFlags) *queue.Result {
			runs++
			pc := runs * 10
			return &queue.Result{
				Info: &flatrpc.ProgInfo{
					Extra: &flatrpc.CallInfo{
						Signal: []uint64{pc},
						Cover:  []uint64{pc},
					},
				},
			}
		})
		assert.False(t, stop)
		return runs, info
	}

	runs, info := runDeflake(true)
	assert.Equal(t, uint64(3), runs)
	assert.ElementsMatch(t, []uint64{1, 10, 20, 30}, info.newSignal.ToRaw())
	assert.ElementsMatch(t, info.newSignal.ToRaw(), info.stableSignal.ToRaw())
	assert.ElementsMatch(t, info.newSignal.ToRaw(), info.newStableSignal.ToRaw())

	runs, info = runDeflake(false)
	assert.Equal(t, uint64(3), runs)
	assert.Empty(t, info.newStableSignal.ToRaw())
	assert.Empty(t, info.stableSignal.ToRaw())
}

// XHyper Extra signal is not per-input reproducible, so handleCall must save
// the program that added max-signal without re-executing it in minimize.
// ProgSmashed keeps this test on the save path: smash does not gate retention.
func TestHandleCallXHyperExtraSavesWithoutMinimize(t *testing.T) {
	target, err := prog.GetTarget(targets.TestOS, targets.TestArch64Fuzz)
	assert.NoError(t, err)
	const anyTestProg = `syz_compare(&AUTO="00000000", 0x4, &AUTO=@conditional={0x0, @void, @void, @void}, AUTO)`
	p, err := target.Deserialize([]byte(anyTestProg), prog.NonStrict)
	assert.NoError(t, err)

	corp := corpus.NewCorpus(context.Background())
	job := &triageJob{
		p:     p,
		flags: ProgSmashed,
		fuzzer: &Fuzzer{
			Config: &Config{
				XHyperCover:    true,
				Corpus:         corp,
				NewInputFilter: func(string) bool { return true },
			},
		},
		info: &JobInfo{},
	}
	info := &triageCall{
		newStableSignal: signal.FromRaw([]uint64{1, 2}, 0),
		stableSignal:    signal.FromRaw([]uint64{1, 2}, 0),
		cover:           cover.FromRaw([]uint64{10, 20}),
	}
	// queue is nil. minimize would enqueue and panic; reaching Save means it was skipped.
	job.handleCall(-1, info)

	items := corp.Items()
	assert.Len(t, items, 1)
	assert.Equal(t, -1, items[0].Call)
	assert.Same(t, p, items[0].Prog)
	assert.ElementsMatch(t, []uint64{1, 2}, items[0].Signal.ToRaw())
	assert.ElementsMatch(t, []uint64{10, 20}, items[0].Cover)
	assert.Equal(t, 1, corp.StatProgs.Val())
	assert.Equal(t, 2, corp.StatCover.Val())
}
