use std::{env, fs, path::Path};

fn main() {
    // Declare expected cfg keys (prevents warnings)
    println!("cargo:rustc-check-cfg=cfg(key_size_24)");

    // First: check the environment override
    let key_size_env = env::var("KEY_SIZE").ok()
        .and_then(|s| s.parse::<usize>().ok());

    // Second: detect KEY_SIZE from constants.rs if no env override
    let key_size = key_size_env.or_else(|| {
        let manifest_dir = env::var("CARGO_MANIFEST_DIR").unwrap();
        let path = Path::new(&manifest_dir).join("constants.rs");
        detect_key_size_from_file(&path)
    });

    match key_size {
        Some(24) => {
            println!("cargo:rustc-cfg=key_size_24");
            eprintln!("Build script: KEY_SIZE = 24 → enabling cfg(key_size_24)");
        }
        Some(other) => {
            eprintln!("Build script: KEY_SIZE = {} → cfg not set", other);
        }
        None => {
            eprintln!("Build script: WARNING: Could not determine KEY_SIZE");
        }
    }
}

fn detect_key_size_from_file(path: &Path) -> Option<usize> {
    let contents = fs::read_to_string(path).ok()?;

    for line in contents.lines() {
        let trimmed = line.trim();

        // Match lines like: pub const KEY_SIZE: usize = 24;
        if trimmed.starts_with("pub const KEY_SIZE")
            && trimmed.contains('=')
        {
            let after_eq = trimmed.split('=').nth(1)?;
            let digits: String = after_eq
                .chars()
                .skip_while(|c| c.is_whitespace())
                .take_while(|c| c.is_ascii_digit())
                .collect();

            if let Ok(n) = digits.parse::<usize>() {
                return Some(n);
            }
        }
    }
    None
}
