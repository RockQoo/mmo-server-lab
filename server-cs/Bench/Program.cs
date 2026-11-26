using BenchmarkDotNet.Running;

// 인자가 없으면 모든 벤치마크를 돌린다. 인자는 BenchmarkDotNet 옵션(예: --filter *ArrayPool*).
BenchmarkSwitcher.FromAssembly(typeof(Program).Assembly).Run(args.Length == 0 ? ["--filter", "*"] : args);
