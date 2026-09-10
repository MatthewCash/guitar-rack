fn main() {
    let qt = pkg_config::Config::new()
        .probe("Qt6Widgets")
        .expect("Qt 6 Widgets development files are required");
    let network = pkg_config::Config::new()
        .probe("Qt6Network")
        .expect("Qt 6 Network development files are required");
    let mut build = cxx_build::bridge("src/ui_bridge.rs");
    build
        .file("src/widgets.cpp")
        .flag_if_supported("-std=c++17");
    for path in qt.include_paths {
        build.include(path);
    }
    for path in network.include_paths {
        build.include(path);
    }
    build.compile("guitar-rack-ui");

    println!("cargo:rerun-if-changed=src/ui_bridge.rs");
    println!("cargo:rerun-if-changed=src/widgets.cpp");
    println!("cargo:rerun-if-changed=src/widgets.h");
}
