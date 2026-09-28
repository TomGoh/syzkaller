// Copyright 2018 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package cover

import (
	"cmp"
	"fmt"
	"slices"
	"sort"
	"strings"

	"github.com/google/syzkaller/pkg/cover/backend"
	"github.com/google/syzkaller/pkg/mgrconfig"
	"github.com/google/syzkaller/pkg/vminfo"
	"github.com/google/syzkaller/sys/targets"
	"golang.org/x/exp/maps"
)

type ReportGenerator struct {
	target          *targets.Target
	srcDir          string
	buildDir        string
	subsystem       []mgrconfig.Subsystem
	rawCoverEnabled bool
	// BlockCoverage is set when XHyperCover is on. Covered PCs are TCG
	// basic-block starts, not __sanitizer_cov_trace_pc callback sites.
	BlockCoverage bool
	*backend.Impl
}

type Prog struct {
	Sig  string
	Data string
	PCs  []uint64
}

func GetPCBase(cfg *mgrconfig.Config) (uint64, error) {
	return backend.GetPCBase(cfg)
}

func MakeReportGenerator(cfg *mgrconfig.Config, modules []*vminfo.KernelModule) (*ReportGenerator, error) {
	impl, err := backend.Make(cfg, modules)
	if err != nil {
		return nil, err
	}
	cfg.KernelSubsystem = append(cfg.KernelSubsystem, mgrconfig.Subsystem{
		Name:  "all",
		Paths: []string{""},
	})
	rg := &ReportGenerator{
		target:          cfg.SysTarget,
		srcDir:          cfg.KernelSrc,
		buildDir:        cfg.KernelBuildSrc,
		subsystem:       cfg.KernelSubsystem,
		rawCoverEnabled: cfg.RawCover,
		BlockCoverage:   cfg.XHyperCover,
		Impl:            impl,
	}
	return rg, nil
}

type file struct {
	module     string
	filename   string
	lines      map[int]line
	functions  []*function
	covered    []backend.Range
	uncovered  []backend.Range
	totalPCs   int
	coveredPCs int
}

type function struct {
	name    string
	pcs     int
	covered int
}

type line struct {
	progCount   map[int]bool   // program indices that cover this line
	progIndex   int            // example program index that covers this line
	pcProgCount map[uint64]int // some lines have multiple BBs
}

type fileMap map[string]*file

func (rg *ReportGenerator) prepareFileMap(progs []Prog, force, debug bool) (fileMap, error) {
	if err := rg.symbolizePCs(uniquePCs(progs...)); err != nil {
		return nil, err
	}
	files := make(fileMap)
	for _, unit := range rg.Units {
		files[unit.Name] = &file{
			module:   unit.Module.Name,
			filename: unit.Path,
			lines:    make(map[int]line),
			totalPCs: len(unit.PCs),
		}
	}
	pcToProgs := make(map[uint64]map[int]bool)
	unmatchedPCs := make(map[uint64]bool)
	for i, prog := range progs {
		for _, pc := range prog.PCs {
			if pcToProgs[pc] == nil {
				pcToProgs[pc] = make(map[int]bool)
			}
			pcToProgs[pc][i] = true
			// Block PCs are not callback sites. Skip the sancov sanity check
			// so the "coverage callbacks" mismatch never fires.
			if rg.BlockCoverage {
				continue
			}
			_, found := slices.BinarySearch(rg.CallbackPoints, pc)
			if rg.PreciseCoverage && !found {
				unmatchedPCs[pc] = true
			}
		}
	}
	err := rg.frame2line(files, pcToProgs, progs)
	if err != nil {
		return nil, err
	}
	// If the backend provided coverage callback locations for the binaries, use them to
	// verify data returned by kcov.
	if len(unmatchedPCs) > 0 && !force {
		return nil, coverageCallbackMismatch(debug, len(pcToProgs), unmatchedPCs)
	}
	if rg.BlockCoverage {
		rg.finishBlockCoverage(files, pcToProgs)
	} else {
		for _, unit := range rg.Units {
			f := files[unit.Name]
			for _, pc := range unit.PCs {
				if pcToProgs[pc] != nil {
					f.coveredPCs++
				}
			}
		}
		for _, s := range rg.Symbols {
			fun := &function{
				name: s.Name,
				pcs:  len(s.PCs),
			}
			for _, pc := range s.PCs {
				if pcToProgs[pc] != nil {
					fun.covered++
				}
			}
			f := files[s.Unit.Name]
			f.functions = append(f.functions, fun)
		}
	}
	for _, f := range files {
		slices.SortFunc(f.functions, func(a, b *function) int {
			return cmp.Compare(a.name, b.name)
		})
	}
	return files, nil
}

// finishBlockCoverage sets per-file totals from DWARF line ranges and
// per-function totals from the STT_FUNC universe. A function is covered when
// any block PC falls in [Start, End), matching xhcov-symbolize.sh. The
// covered/total ratio inside a function is how many of its line ranges were hit.
func (rg *ReportGenerator) finishBlockCoverage(files fileMap, pcToProgs map[uint64]map[int]bool) {
	for _, f := range files {
		n := len(f.covered) + len(f.uncovered)
		if n == 0 {
			continue
		}
		f.totalPCs = n
		f.coveredPCs = len(f.covered)
	}
	pcs := coveredPCList(pcToProgs)
	lineStat := make(map[*backend.Symbol][2]int) // [0] line ranges, [1] ranges hit
	for _, frame := range rg.Frames {
		if frame.PCEnd <= frame.PC || frame.StartLine <= 0 {
			continue
		}
		sym := backend.ContainingSymbol(rg.Symbols, frame.PC)
		if sym == nil {
			continue
		}
		st := lineStat[sym]
		st[0]++
		if pcInRange(pcs, frame.PC, frame.PCEnd) {
			st[1]++
		}
		lineStat[sym] = st
	}
	hit := make(map[*backend.Symbol]bool)
	for _, pc := range pcs {
		if sym := backend.ContainingSymbol(rg.Symbols, pc); sym != nil {
			hit[sym] = true
		}
	}
	for _, sym := range rg.Symbols {
		if sym.Unit == nil {
			continue
		}
		st := lineStat[sym]
		total := st[0]
		covered := st[1]
		if total == 0 {
			total = 1
		}
		// A block PC in the symbol range covers the function even when it
		// lands between line entries (padding). The numerator stays within
		// the line-range denominator.
		if hit[sym] && covered == 0 {
			covered = 1
		}
		if covered > total {
			covered = total
		}
		f := files[sym.Unit.Name]
		if f == nil {
			modName := ""
			if sym.Module != nil {
				modName = sym.Module.Name
			}
			f = &file{
				module:   modName,
				filename: sym.Unit.Path,
				lines:    make(map[int]line),
			}
			files[sym.Unit.Name] = f
		}
		f.functions = append(f.functions, &function{
			name:    sym.Name,
			pcs:     total,
			covered: covered,
		})
	}
}

func coveredPCList(pcToProgs map[uint64]map[int]bool) []uint64 {
	pcs := make([]uint64, 0, len(pcToProgs))
	for pc := range pcToProgs {
		pcs = append(pcs, pc)
	}
	slices.Sort(pcs)
	return pcs
}

func pcInRange(sorted []uint64, start, end uint64) bool {
	if start >= end || len(sorted) == 0 {
		return false
	}
	i := sort.Search(len(sorted), func(i int) bool { return sorted[i] >= start })
	return i < len(sorted) && sorted[i] < end
}

func progsCoveringRange(sorted []uint64, pcToProgs map[uint64]map[int]bool, start, end uint64) (map[int]bool, []uint64) {
	if start >= end || len(sorted) == 0 {
		return nil, nil
	}
	i := sort.Search(len(sorted), func(i int) bool { return sorted[i] >= start })
	var progs map[int]bool
	var hit []uint64
	for ; i < len(sorted) && sorted[i] < end; i++ {
		pc := sorted[i]
		hit = append(hit, pc)
		for progIndex := range pcToProgs[pc] {
			if progs == nil {
				progs = make(map[int]bool)
			}
			progs[progIndex] = true
		}
	}
	return progs, hit
}

func (rg *ReportGenerator) frame2line(files fileMap, pcToProgs map[uint64]map[int]bool, progs []Prog) error {
	if rg.BlockCoverage {
		return rg.frame2lineBlock(files, pcToProgs, progs)
	}
	matchedPC := false
	for _, frame := range rg.Frames {
		if frame.StartLine < 0 {
			continue
		}
		f := fileByFrame(files, frame)
		ln := f.lines[frame.StartLine]
		coveredBy := pcToProgs[frame.PC]
		if len(coveredBy) == 0 {
			f.uncovered = append(f.uncovered, frame.Range)
			continue
		}
		// Covered frame.
		f.covered = append(f.covered, frame.Range)
		matchedPC = true
		if ln.progCount == nil {
			ln.progCount = make(map[int]bool)
			ln.pcProgCount = make(map[uint64]int)
			ln.progIndex = -1
		}
		for progIndex := range coveredBy {
			ln.progCount[progIndex] = true
			if ln.progIndex == -1 || len(progs[progIndex].Data) < len(progs[ln.progIndex].Data) {
				ln.progIndex = progIndex
			}
			ln.pcProgCount[frame.PC]++
		}
		f.lines[frame.StartLine] = ln
	}
	if !matchedPC {
		return fmt.Errorf("coverage doesn't match any coverage callbacks")
	}
	return nil
}

// frame2lineBlock marks a source line covered when a block-start PC falls in
// the line's instruction range [frame.PC, frame.PCEnd). Exact frame.PC equality
// is the kcov/sancov path; block starts usually sit strictly inside the range.
func (rg *ReportGenerator) frame2lineBlock(files fileMap, pcToProgs map[uint64]map[int]bool, progs []Prog) error {
	matchedPC := false
	pcs := coveredPCList(pcToProgs)
	for _, frame := range rg.Frames {
		if frame.StartLine <= 0 || frame.PCEnd <= frame.PC {
			continue
		}
		f := fileByFrame(files, frame)
		coveredBy, hitPCs := progsCoveringRange(pcs, pcToProgs, frame.PC, frame.PCEnd)
		if len(coveredBy) == 0 {
			f.uncovered = append(f.uncovered, frame.Range)
			continue
		}
		f.covered = append(f.covered, frame.Range)
		matchedPC = true
		ln := f.lines[frame.StartLine]
		if ln.progCount == nil {
			ln.progCount = make(map[int]bool)
			ln.pcProgCount = make(map[uint64]int)
			ln.progIndex = -1
		}
		for progIndex := range coveredBy {
			ln.progCount[progIndex] = true
			if ln.progIndex == -1 || len(progs[progIndex].Data) < len(progs[ln.progIndex].Data) {
				ln.progIndex = progIndex
			}
		}
		for _, pc := range hitPCs {
			ln.pcProgCount[pc] += len(pcToProgs[pc])
		}
		f.lines[frame.StartLine] = ln
	}
	if !matchedPC {
		return fmt.Errorf("coverage doesn't match any DWARF line ranges")
	}
	return nil
}

func coverageCallbackMismatch(debug bool, numPCs int, unmatchedPCs map[uint64]bool) error {
	var debugStr strings.Builder
	if debug {
		debugStr.WriteString("\n\nUnmatched PCs:\n")
		for pc := range unmatchedPCs {
			debugStr.WriteString(fmt.Sprintf("%x\n", pc))
		}
	}
	return fmt.Errorf("%d out of %d PCs returned by kcov do not have matching coverage callbacks."+
		" Check the discoverModules() code. Use ?force=1 to disable this message.%s",
		len(unmatchedPCs), numPCs, debugStr.String())
}

func uniquePCs(progs ...Prog) []uint64 {
	PCs := make(map[uint64]bool)
	for _, p := range progs {
		for _, pc := range p.PCs {
			PCs[pc] = true
		}
	}
	return maps.Keys(PCs)
}

func (rg *ReportGenerator) symbolizePCs(PCs []uint64) error {
	if len(PCs) == 0 {
		return fmt.Errorf("no coverage collected so far to symbolize")
	}
	if len(rg.Symbols) == 0 {
		return nil
	}
	symbolize := make(map[*backend.Symbol]bool)
	pcs := make(map[*vminfo.KernelModule][]uint64)
	for _, pc := range PCs {
		sym := rg.findSymbol(pc)
		if sym == nil || sym.Symbolized || symbolize[sym] {
			continue
		}
		symbolize[sym] = true
		pcs[sym.Module] = append(pcs[sym.Module], sym.PCs...)
	}
	if len(symbolize) == 0 {
		return nil
	}
	frames, err := rg.Symbolize(pcs)
	if err != nil {
		return err
	}
	rg.Frames = append(rg.Frames, frames...)
	for sym := range symbolize {
		sym.Symbolized = true
	}
	return nil
}

func fileByFrame(files map[string]*file, frame *backend.Frame) *file {
	f := files[frame.Name]
	if f == nil {
		f = &file{
			module:   frame.Module.Name,
			filename: frame.Path,
			lines:    make(map[int]line),
			// Special mark for header files, if a file does not have coverage at all it is not shown.
			totalPCs:   1,
			coveredPCs: 1,
		}
		files[frame.Name] = f
	}
	return f
}

func (rg *ReportGenerator) findSymbol(pc uint64) *backend.Symbol {
	idx := sort.Search(len(rg.Symbols), func(i int) bool {
		return pc < rg.Symbols[i].End
	})
	if idx == len(rg.Symbols) {
		return nil
	}
	s := rg.Symbols[idx]
	if pc < s.Start || pc > s.End {
		return nil
	}
	return s
}
