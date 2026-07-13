// Root module for compress-utils. The Go binding lives in bindings/go/ and is
// imported as github.com/dupontcyborg/compress-utils/bindings/go — but the
// module must be rooted here (not under bindings/go) so cgo can compile the
// vendored codec sources under third_party/: a nested module would exclude
// third_party from the downloaded module, and cgo only compiles C within the
// module. See docs/adding-a-language.md for the root-manifest convention.
module github.com/dupontcyborg/compress-utils

go 1.21
