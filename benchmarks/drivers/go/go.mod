// The Go benchmark driver, as its own module: it needs competitor libraries that
// must not appear in the root module's go.mod (the published binding). The replace
// builds compress-utils from this checkout, C core included, exactly as a consumer
// of the root module would.
module bench-go

go 1.25

require github.com/dupontcyborg/compress-utils v0.0.0

require (
	github.com/andybalholm/brotli v1.2.7 // indirect
	github.com/dsnet/compress v0.0.1 // indirect
	github.com/klauspost/compress v1.20.3 // indirect
	github.com/pierrec/lz4/v4 v4.1.33 // indirect
	github.com/ulikunitz/xz v0.5.17 // indirect
)

replace github.com/dupontcyborg/compress-utils => ../../..
