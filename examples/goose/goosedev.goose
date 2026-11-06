// The device goosedev (goosedev.ent), in Goose: memory-safe, no heap, made
// into C by `goose` (build.sh). Its functions are exported under goose_
// names; goosedev_shim.c gives ent its ent_goosedev_ ones.
//
// A Goose program is one instance, its data stacks its own: a function
// that keeps an array on one (`square_sum`, `median`) may only be called from one thread
// at a time, so ent declares it an `extern proc`, and its purity check
// says so where it is declared a fn. One that keeps nothing there
// (`mix`) is a fn, called from any thread.

import std;

// splitmix64's finish: no data stack.
export "goose_mix" fn mix(x: u64) -> u64 {
    var z = x + 0x9E3779B97F4A7C15;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EB;
    return z ^ (z >> 31);
}

// The squares below n added up, by way of an array on the data stack.
export "goose_square_sum" fn square_sum(n: i32) -> i64 {
    var squares: i64[>..] = [];
    for i in 0..n {
        let w: i64 = i;
        squares.push(w * w);
    }
    var sum: i64 = 0;
    for s in squares { sum += s; }
    return sum;
}

// The whole numbers of comma-separated text, written into `into` in their
// order: how many there are; -1 at a byte that is neither a digit nor a
// comma, -2 where they are more than `into` holds. What it is given is
// checked, so a bad file is an answer, not a stop (a failed check in an
// exported function ends the program).
export "goose_parse" fn parse(text: const u8[:], into: i64[:]) -> i64 {
    var count: i64 = 0;
    var current: i64 = 0;
    var digits = false;
    for c in text {
        if c == ',' {
            if count == into.len { return -2; }
            into[count] = current;
            count++;
            current = 0;
            digits = false;
        } else if c >= '0' && c <= '9' {
            current = current * 10 + (c - '0');
            digits = true;
        } else {
            return -1;
        }
    }
    if digits {
        if count == into.len { return -2; }
        into[count] = current;
        count++;
    }
    return count;
}

// The median of the values (the lower of the middle two where they are
// even), by a sorted copy on the data stack; 0 where there are none.
export "goose_median" fn median(values: const i64[:]) -> i64 {
    if values.len == 0 { return 0; }
    var copy: i64[>..] = [];
    for v in values { copy.push(v); }
    sort(copy[..]);
    return copy[(copy.len - 1) / 2];
}
