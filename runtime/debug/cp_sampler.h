// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// NFSMW_CP_SAMPLE=<file>[,<from s>[,<seconds>]] (Windows): a sampling profile
// of the command processor, the thread that bounds the race on a fast GPU.
// From that many seconds after launch (default 60) for that long (10), a
// thread of its own stops it every 0.5 ms, unwinds its stack (the x64 unwind
// tables) and counts the stack. At the end it writes <file>: one line per
// distinct stack, "<count> <module>+0x<rva> ..." innermost first, for
// llvm-symbolizer (--obj=<the unstripped .exe>, address 0x140000000 + rva).
// Linux and Android have perf and simpleperf for this instead.
#pragma once

namespace cpsampler
{
    // The calling thread is the one sampled (once; later calls do nothing).
    void StartForCurrentThread();
}
