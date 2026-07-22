// Copyright 2024 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package backend

import (
	"testing"
)

func TestGetTraceCallbackType(t *testing.T) {
	inputData := map[int][]string{
		TraceCbNone: {
			"foobar",
			"___sanitizer_cov_trace_pc",
		},
		TraceCbPc: {
			"__sanitizer_cov_trace_pc",
			"____sanitizer_cov_trace_pc_veneer",
			// pKVM EL2 (Rust hyp) coverage callback, __kvm_nvhe_-prefixed by the nVHE linker.
			"__kvm_nvhe___sanitizer_cov_trace_pc",
		},
		TraceCbCmp: {
			"__sanitizer_cov_trace_cmp1",
			"__sanitizer_cov_trace_const_cmp4",
			"____sanitizer_cov_trace_const_cmp4_veneer",
		},
	}
	for expected, names := range inputData {
		for _, name := range names {
			result := getTraceCallbackType(name)
			if result != expected {
				t.Fatalf("getTraceCallbackType(`%v`) unexpectedly returned %v", name, result)
			}
		}
	}
}
