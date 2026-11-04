// The declarations ent made for the device (ent_extern.h, in the
// ENT_INCLUDE that tools/ent sets as it builds) as Rust: src/lib.rs
// checks its definitions against them.
fn main() {
    let include = std::env::var("ENT_INCLUDE").expect("ENT_INCLUDE (set by tools/ent)");
    println!("cargo:rerun-if-env-changed=ENT_INCLUDE");
    println!("cargo:rerun-if-changed={include}/ent_extern.h");
    let out = std::path::PathBuf::from(std::env::var("OUT_DIR").unwrap()).join("ent.rs");
    bindgen::Builder::default()
        .header(format!("{include}/ent_extern.h"))
        .allowlist_function("ent_rustdev_.*")
        .generate()
        .expect("ent_extern.h made into Rust (bindgen needs libclang: LIBCLANG_PATH)")
        .write_to_file(out)
        .unwrap();
}
