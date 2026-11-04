//! The functions of the device rustdev (rustdev.ent), in Rust: each
//! exported under the name the program calls (`ent_` and the module's and
//! its own) with C's calling convention. Each is checked as it compiles
//! against the declaration ent made of it (build.rs), so that one changed
//! without the other is an error here and not a wrong call.
//!
//! A panic aborts the program (Cargo.toml), as it may not unwind into
//! ent. And a fn must give the same result for the same arguments: not so
//! a walk over a HashMap, whose order std chooses anew for each map.
#![allow(non_camel_case_types, non_upper_case_globals, dead_code)]

mod ent {
    include!(concat!(env!("OUT_DIR"), "/ent.rs"));
}

/// Fails to compile where `def` has another signature than ent's `decl`.
macro_rules! same_signature {
    ($decl:path, $def:path) => {
        const _: () = {
            let _ = if true { $decl } else { $def };
        };
    };
}

/// A whole number mixed (splitmix64's finish): pure, called from a `for`
/// over entities on many threads.
#[no_mangle]
pub extern "C" fn ent_rustdev_mix(x: u64) -> u64 {
    let mut z = x.wrapping_add(0x9E3779B97F4A7C15);
    z = (z ^ (z >> 30)).wrapping_mul(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)).wrapping_mul(0x94D049BB133111EB);
    z ^ (z >> 31)
}
same_signature!(ent::ent_rustdev_mix, ent_rustdev_mix);

/// The squares below n added up, by way of a Vec: Rust's allocator on the
/// program's threads.
#[no_mangle]
pub extern "C" fn ent_rustdev_square_sum(n: i32) -> i64 {
    let values: Vec<i64> = (0..n as i64).map(|i| i * i).collect();
    values.iter().sum()
}
same_signature!(ent::ent_rustdev_square_sum, ent_rustdev_square_sum);

/// a / b, and -1 where b is nought.
#[no_mangle]
pub extern "C" fn ent_rustdev_safe_div(a: i64, b: i64) -> i64 {
    a.checked_div(b).unwrap_or(-1)
}
same_signature!(ent::ent_rustdev_safe_div, ent_rustdev_safe_div);

/// The sum of x * y over a buffer's rows (handed as how many, then where
/// each field's values are).
#[no_mangle]
pub unsafe extern "C" fn ent_rustdev_dot(count: i32, x: *const f64, y: *const f64) -> f64 {
    let n = count as usize;
    let (x, y) = (std::slice::from_raw_parts(x, n), std::slice::from_raw_parts(y, n));
    x.iter().zip(y).map(|(a, b)| a * b).sum()
}
same_signature!(ent::ent_rustdev_dot, ent_rustdev_dot);

/// A line said by Rust's println.
#[no_mangle]
pub extern "C" fn ent_rustdev_greet(n: i32) {
    println!("Rust says hello, {n}");
}
same_signature!(ent::ent_rustdev_greet, ent_rustdev_greet);

/// The colours of the .png `path`, as stb_image gives them to
/// pictures.read_picture: 0xRRGGBB, a grey one's in all three, how much
/// shows (alpha) dropped, 16 bits a sample cut to their high 8.
fn decode(path: &str, color: &mut [i32]) -> Result<(i32, usize), i32> {
    let file = std::fs::File::open(path).map_err(|_| -1)?;
    let mut decoder = png::Decoder::new(std::io::BufReader::new(file));
    decoder.set_transformations(png::Transformations::normalize_to_color8());
    let mut reader = decoder.read_info().map_err(|_| -1)?;
    let (width, height) = (reader.info().width as usize, reader.info().height as usize);
    if width * height > color.len() {
        return Err(-2);
    }
    let mut pixels = vec![0; reader.output_buffer_size()];
    let frame = reader.next_frame(&mut pixels).map_err(|_| -1)?;
    let channels = frame.color_type.samples();
    for (c, p) in color.iter_mut().zip(pixels[..frame.buffer_size()].chunks_exact(channels)) {
        let (r, g, b) = if channels < 3 { (p[0], p[0], p[0]) } else { (p[0], p[1], p[2]) };
        *c = (r as i32) << 16 | (g as i32) << 8 | b as i32;
    }
    Ok((width as i32, width * height))
}

/// The .png `name` read into `into` (how many it has, how many it holds,
/// where its colours are), in place of what that had: how many pixels
/// wide it is, -1 where there is no such file or it is no .png, -2 where
/// it has more pixels than `into` holds (`into` is empty then).
#[no_mangle]
pub unsafe extern "C" fn ent_rustdev_read_png(
    name: *const ent::ent_text126,
    count: *mut i32,
    capacity: i32,
    color: *mut i32,
) -> i32 {
    let name = &*name;
    let bytes = std::slice::from_raw_parts(name.bytes.as_ptr() as *const u8, name.length as usize);
    *count = 0;
    let Ok(path) = std::str::from_utf8(bytes) else { return -1 };
    match decode(path, std::slice::from_raw_parts_mut(color, capacity as usize)) {
        Ok((width, n)) => {
            *count = n as i32;
            width
        }
        Err(e) => e,
    }
}
same_signature!(ent::ent_rustdev_read_png, ent_rustdev_read_png);
