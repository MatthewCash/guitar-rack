#[cxx::bridge]
mod ffi {
    unsafe extern "C++" {
        include!("guitar-rack/src/widgets.h");

        fn run_guitar_rack(socket_path: &CxxString) -> i32;
    }
}

pub fn run(socket_path: &str) -> i32 {
    cxx::let_cxx_string!(path = socket_path);
    ffi::run_guitar_rack(&path)
}
