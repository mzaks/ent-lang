//! The extern fns and procs of externs_rust.ent, some of which keep their
//! word and some of which do not.
use std::collections::HashMap;
use std::sync::atomic::{AtomicI64, Ordering};

static CALLS: AtomicI64 = AtomicI64::new(0);
static mut SCALE: i64 = 1;

/// The squares below n added up, by way of a Vec: memory had and given
/// back.
#[no_mangle]
pub extern "C" fn ent_square_sum(n: i32) -> i64 {
    let values: Vec<i64> = (0..n as i64).map(|i| i * i).collect();
    values.iter().sum()
}

/// a / b, which panics (and so ends the program) where b is nought.
#[no_mangle]
pub extern "C" fn ent_divided(a: i64, b: i64) -> i64 {
    if b == 0 {
        panic!("division by nought");
    }
    a / b
}

/// The sum of a HashMap's keys: the same whichever order they come in,
/// as the static below vouches.
#[no_mangle]
pub static ent_same_result_ent_key_sum: u8 = 0;
#[no_mangle]
pub extern "C" fn ent_key_sum(n: i32) -> i64 {
    let map: HashMap<i32, i32> = (0..n).map(|i| (i, i)).collect();
    map.keys().map(|&k| k as i64).sum()
}

/// x and how many calls came before.
#[no_mangle]
pub extern "C" fn ent_counted(x: i64) -> i64 {
    x + CALLS.fetch_add(1, Ordering::Relaxed)
}

/// x times what set_scale set.
#[no_mangle]
pub extern "C" fn ent_scaled(x: i64) -> i64 {
    x * unsafe { SCALE }
}

/// The first key a HashMap of 0..n gives: std chooses its order anew for
/// each map.
#[no_mangle]
pub extern "C" fn ent_first_key(n: i32) -> i32 {
    let map: HashMap<i32, i32> = (0..n).map(|i| (i, i)).collect();
    *map.keys().next().unwrap_or(&-1)
}

#[no_mangle]
pub extern "C" fn ent_set_scale(s: i64) {
    unsafe { SCALE = s }
}
