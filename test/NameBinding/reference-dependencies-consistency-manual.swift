// Some types, such as StringLiteralType, used to be cached in the TypeChecker.
// Consequently, the second primary file (in batch mode) to use that type would
// hit in the cache and no dependency would be recorded.
// This test ensures that this bug stays fixed.
//
// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/request-based)
// RUN: %empty-directory(%t/manual)
//
// Create two identical inputs, each needing StringLiteralType:
//
// RUN: echo 'fileprivate var v: String { return "\(x)" }; fileprivate let x = "a"' >%t/1.swift
// RUN: echo 'fileprivate var v: String { return "\(x)" }; fileprivate let x = "a"' >%t/2.swift
//
// RUN:  %target-swift-frontend -enable-fine-grained-dependencies -typecheck -primary-file %t/1.swift -primary-file %t/2.swift -emit-reference-dependencies-path %t/request-based/1.swiftdeps -emit-reference-dependencies-path %t/request-based/2.swiftdeps
// RUN:  %target-swift-frontend -enable-fine-grained-dependencies -typecheck -primary-file %t/1.swift -primary-file %t/2.swift -emit-reference-dependencies-path %t/manual/1.swiftdeps -emit-reference-dependencies-path %t/manual/2.swiftdeps -disable-request-based-incremental-dependencies
//
// Sequence numbers can vary
// RUN: sed -e 's/[0-9][0-9]*/N/g' -e 's/N, //g' -e '/^ *$/d' <%t/request-based/1.swiftdeps | sort >%t/request-based/1-processed.swiftdeps
// RUN: sed -e 's/[0-9][0-9]*/N/g' -e 's/N, //g' -e '/^ *$/d' <%t/request-based/2.swiftdeps | sort >%t/request-based/2-processed.swiftdeps
// RUN: sed -e 's/[0-9][0-9]*/N/g' -e 's/N, //g' -e '/^ *$/d' <%t/manual/1.swiftdeps | sort >%t/manual/1-processed.swiftdeps
// RUN: sed -e 's/[0-9][0-9]*/N/g' -e 's/N, //g' -e '/^ *$/d' <%t/manual/2.swiftdeps | sort >%t/manual/2-processed.swiftdeps

// Check that manual matches manual and request-based matches request-based.
// Unfortunately, the remaining two cross-configuration tests will not pass
// because request-based dependencies capture strictly more dependency edges
// than the manual name tracker.
// RUN: cmp -s %t/request-based/1-processed.swiftdeps %t/request-based/2-processed.swiftdeps
// RUN: cmp -s %t/manual/1-processed.swiftdeps %t/manual/2-processed.swiftdeps
