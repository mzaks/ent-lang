//! The workloads of bench/devices in Rust, as a Rust programmer writes
//! them: slices made of what ent hands, a Vec where an array grows, an
//! array on the stack where it does not. What dev.ent declares.
use std::slice;

#[no_mangle]
pub extern "C" fn ent_dev_mix(x: u64) -> u64 {
    let mut z = x.wrapping_add(0x9E3779B97F4A7C15);
    z = (z ^ (z >> 30)).wrapping_mul(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)).wrapping_mul(0x94D049BB133111EB);
    z ^ (z >> 31)
}

#[no_mangle]
pub extern "C" fn ent_dev_square_sum(n: i32) -> i64 {
    let squares: Vec<i64> = (0..n as i64).map(|i| i * i).collect();
    squares.iter().sum()
}

#[no_mangle]
pub extern "C" fn ent_dev_median16(seed: u64) -> u64 {
    let mut a = [0u64; 16];
    for (k, v) in a.iter_mut().enumerate() {
        *v = ent_dev_mix(seed.wrapping_mul(16).wrapping_add(k as u64));
    }
    for i in 1..16 {
        let v = a[i];
        let mut j = i;
        while j > 0 && a[j - 1] > v {
            a[j] = a[j - 1];
            j -= 1;
        }
        a[j] = v;
    }
    a[7]
}

#[no_mangle]
pub unsafe extern "C" fn ent_dev_parse(
    length: i32, text: *const u8, count: *mut i32, room: i32, into: *mut i64,
) -> i64 {
    let text = slice::from_raw_parts(text, length as usize);
    let into = slice::from_raw_parts_mut(into, room as usize);
    let (mut n, mut current, mut digits) = (0usize, 0i64, false);
    for &c in text {
        match c {
            b',' => {
                if n == into.len() { *count = n as i32; return -2; }
                into[n] = current;
                n += 1;
                current = 0;
                digits = false;
            }
            b'0'..=b'9' => {
                current = current * 10 + (c - b'0') as i64;
                digits = true;
            }
            _ => { *count = 0; return -1; }
        }
    }
    if digits {
        if n == into.len() { *count = n as i32; return -2; }
        into[n] = current;
        n += 1;
    }
    *count = n as i32;
    n as i64
}

#[no_mangle]
pub unsafe extern "C" fn ent_dev_median(
    n: i32, values: *const i64, count: *mut i32, room: i32, scratch: *mut i64,
) -> i64 {
    let n = n.min(room) as usize;
    let s = slice::from_raw_parts_mut(scratch, n);
    s.copy_from_slice(slice::from_raw_parts(values, n));
    *count = n as i32;
    if n == 0 { return 0; }
    let k = ((n - 1) / 2) as i64;
    let (mut lo, mut hi) = (0i64, n as i64 - 1);
    while lo < hi {
        let p = s[((lo + hi) / 2) as usize];
        let (mut i, mut j) = (lo, hi);
        while i <= j {
            while s[i as usize] < p { i += 1; }
            while s[j as usize] > p { j -= 1; }
            if i <= j {
                s.swap(i as usize, j as usize);
                i += 1;
                j -= 1;
            }
        }
        if k <= j { hi = j; } else if k >= i { lo = i; } else { break; }
    }
    s[k as usize]
}

#[no_mangle]
pub unsafe extern "C" fn ent_dev_blur(
    n: i32, from: *const u8, count: *mut i32, room: i32, to: *mut u8, width: i32,
) {
    let n = n.min(room) as usize;
    let w = width as usize;
    let from = slice::from_raw_parts(from, n);
    let to = slice::from_raw_parts_mut(to, n);
    let height = n / w;
    for y in 0..height {
        for x in 0..w {
            let at = y * w + x;
            if y == 0 || x == 0 || y == height - 1 || x == w - 1 {
                to[at] = from[at];
            } else {
                let mut sum = 0u32;
                for row in [at - w, at, at + w] {
                    sum += from[row - 1] as u32 + from[row] as u32 + from[row + 1] as u32;
                }
                to[at] = (sum / 9) as u8;
            }
        }
    }
    *count = (height * w) as i32;
}
