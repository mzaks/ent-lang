// The workloads of bench/devices in Goose, as a Goose programmer writes
// them: a growing array on the data stack, a fixed one on the native
// stack, slices of what ent hands. What dev.ent declares; dev_shim.c calls
// them under ent's names.

import std;

export "goose_mix" fn mix(x: u64) -> u64 {
    var z = x + 0x9E3779B97F4A7C15;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EB;
    return z ^ (z >> 31);
}

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

export "goose_median16" fn median16(seed: u64) -> u64 {
    var a: u64[16] = [0; 16];
    for k in 0..16 { a[k] = mix(seed * 16 + k as u64); }
    for i in 1..16 {
        let v = a[i];
        var j = i;
        while j > 0 && a[j - 1] > v {
            a[j] = a[j - 1];
            j--;
        }
        a[j] = v;
    }
    return a[7];
}

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

export "goose_median" fn median(values: const i64[:], s: i64[:]) -> i64 {
    let n = values.len;
    for i in 0..n { s[i] = values[i]; }
    if n == 0 { return 0; }
    let k = (n - 1) / 2;
    var lo: i64 = 0;
    var hi = n - 1;
    while lo < hi {
        let p = s[(lo + hi) / 2];
        var i = lo;
        var j = hi;
        while i <= j {
            while s[i] < p { i++; }
            while s[j] > p { j--; }
            if i <= j {
                let t = s[i];
                s[i] = s[j];
                s[j] = t;
                i++;
                j--;
            }
        }
        if k <= j {
            hi = j;
        } else if k >= i {
            lo = i;
        } else {
            break;
        }
    }
    return s[k];
}

export "goose_blur" fn blur(from: const u8[:], to: u8[:], width: i64) {
    let height = from.len / width;
    for y in 0..height {
        for x in 0..width {
            let at = y * width + x;
            if y == 0 || x == 0 || y == height - 1 || x == width - 1 {
                to[at] = from[at];
            } else {
                var sum: u32 = 0;
                for dy in 0..3 {
                    let row = at + (dy - 1) * width;
                    sum += from[row - 1] as u32 + from[row] + from[row + 1];
                }
                to[at] = (sum / 9) as u8;
            }
        }
    }
}
