// Copyright 2020 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

// Package backend provides coverage symbolization backends for DWARF, ELF, Mach-O, and gVisor executables.
package backend

import (
	"fmt"
	"sort"

	"github.com/google/syzkaller/pkg/mgrconfig"
	"github.com/google/syzkaller/pkg/vminfo"
	"github.com/google/syzkaller/sys/targets"
)

type Impl struct {
	Units           []*CompileUnit
	Symbols         []*Symbol
	Frames          []*Frame
	Symbolize       func(pcs map[*vminfo.KernelModule][]uint64) ([]*Frame, error)
	CallbackPoints  []uint64
	PreciseCoverage bool
}

type CompileUnit struct {
	ObjectUnit
	Path   string
	Module *vminfo.KernelModule
}

type Symbol struct {
	ObjectUnit
	Module     *vminfo.KernelModule
	Unit       *CompileUnit
	Start      uint64
	End        uint64
	Symbolized bool
}

// ObjectUnit represents either CompileUnit or Symbol.
type ObjectUnit struct {
	Name string
	PCs  []uint64 // PCs we can get in coverage callbacks for this unit.
	CMPs []uint64 // PCs we can get in comparison interception callbacks for this unit.
}

type Frame struct {
	Module *vminfo.KernelModule
	PC     uint64
	// PCEnd is the exclusive end of this frame's instruction range.
	// Zero means the frame is a single coverage-callback PC (exact match on PC).
	// Block coverage sets [PC, PCEnd) from the DWARF line-number program.
	PCEnd    uint64
	Name     string
	FuncName string
	Path     string
	Inline   bool
	Range
}

// ContainingSymbol returns the symbol with the greatest Start <= pc whose
// half-open range [Start, End) contains pc. symbols must be sorted by Start.
// A PC that falls past that symbol's End is not attributed to an earlier
// overlapping symbol — the same rule as xhcov-symbolize.sh functions_hit.
func ContainingSymbol(symbols []*Symbol, pc uint64) *Symbol {
	if len(symbols) == 0 {
		return nil
	}
	i := sort.Search(len(symbols), func(i int) bool {
		return symbols[i].Start > pc
	}) - 1
	if i < 0 {
		return nil
	}
	s := symbols[i]
	if pc < s.Start || pc >= s.End {
		return nil
	}
	return s
}

type Range struct {
	StartLine int
	StartCol  int
	EndLine   int
	EndCol    int
}

type SecRange struct {
	Start uint64
	End   uint64
}

const LineEnd = 1 << 30

func Make(cfg *mgrconfig.Config, modules []*vminfo.KernelModule) (*Impl, error) {
	kernelDirs := cfg.KernelDirs()
	target := cfg.SysTarget
	moduleObj := cfg.ModuleObj
	vm := cfg.Type
	if kernelDirs.Obj == "" {
		return nil, fmt.Errorf("kernel obj directory is not specified")
	}
	if target.OS == targets.Darwin {
		return makeMachO(target, kernelDirs, moduleObj, modules)
	}
	if vm == targets.GVisor {
		return makeGvisor(target, kernelDirs, modules)
	}
	var delimiters []string
	if cfg.AndroidSplitBuild {
		// Path prefixes used by Android Pixel kernels. See
		// https://source.android.com/docs/setup/build/building-pixel-kernels for more
		// details.
		delimiters = []string{"/aosp/", "/private/"}
	}
	return makeELF(target, kernelDirs, delimiters, moduleObj, modules, cfg.XHyperCover)
}

func GetPCBase(cfg *mgrconfig.Config) (uint64, error) {
	if cfg.Target.OS == targets.Linux && cfg.Type != targets.GVisor && cfg.Type != targets.Starnix {
		return getLinuxPCBase(cfg)
	}
	return 0, nil
}
