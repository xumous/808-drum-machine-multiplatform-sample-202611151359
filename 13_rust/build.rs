// ============================================================================
//  build.rs —— 构建脚本（作者：徐某）
//
//  唯一职责：如果工程根目录存在 app.res（由 app.rc 编译出的资源文件，
//  里面装的是 app.ico 图标），就把它塞给链接器。
//  MSVC 的 link.exe 直接接受 .res 文件，命令等价于：
//      link ... app.res
//  这样 EXE 就带上了自己的图标，不需要任何额外工具链。
//
//  注意：build.rs 只用 std::process / std::path，绝不引入 crate。
// ============================================================================
use std::path::PathBuf;
use std::process::Command;

fn main() {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let rc_file = root.join("app.rc");
    let res_file = root.join("app.res");

    // 若资源文件不存在或 app.rc 更新过，就用 Windows SDK 的 rc.exe 重新编译一次。
    // 找不到 rc.exe 也没关系：程序照样能编过，只是没有自定义图标。
    let need_build = match (fs_modified(&rc_file), fs_modified(&res_file)) {
        (Some(a), Some(b)) => a > b,
        (Some(_), None) => true,
        _ => false,
    };
    if need_build {
        if let Some(rc) = find_rc() {
            let mut cmd = Command::new(rc);
            cmd.current_dir(&root).arg("/nologo");
            // rc.exe 也需要找到 windows.h / winver.h，把 SDK 的 include 目录显式喂给它
            for inc in sdk_includes() {
                cmd.arg("/I").arg(inc);
            }
            let st = cmd.arg("/fo").arg(&res_file).arg(&rc_file).status();
            println!("cargo:warning=rc.exe status: {:?}", st.map(|s| s.success()));
        }
    }

    if res_file.exists() {
        // 把 .res 当作一个额外的链接输入交给 link.exe
        println!("cargo:rustc-link-arg={}", res_file.display());
    }

    println!("cargo:rerun-if-changed=app.rc");
    println!("cargo:rerun-if-changed=app.ico");
}

/// 文件修改时间（拿不到就返回 None）
fn fs_modified(p: &PathBuf) -> Option<std::time::SystemTime> {
    std::fs::metadata(p).ok().and_then(|m| m.modified().ok())
}

/// 在 Windows Kits 的常见版本目录里找 rc.exe（版本号不写死，取最新的一个）。
fn find_rc() -> Option<PathBuf> {
    let base = PathBuf::from(r"C:\Program Files (x86)\Windows Kits\10\bin");
    let mut versions: Vec<PathBuf> = std::fs::read_dir(&base)
        .ok()?
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| p.is_dir())
        .collect();
    versions.sort();
    versions.reverse();
    for v in versions {
        let exe = v.join("x64").join("rc.exe");
        if exe.exists() {
            return Some(exe);
        }
    }
    None
}

/// 收集 SDK 的 Include 目录（um / shared / ucrt），供 rc.exe 找 windows.h
fn sdk_includes() -> Vec<PathBuf> {
    let mut out = Vec::new();
    let base = PathBuf::from(r"C:\Program Files (x86)\Windows Kits\10\Include");
    let mut versions: Vec<PathBuf> = match std::fs::read_dir(&base) {
        Ok(rd) => rd.filter_map(|e| e.ok()).map(|e| e.path()).filter(|p| p.is_dir()).collect(),
        Err(_) => return out,
    };
    versions.sort();
    versions.reverse();
    if let Some(v) = versions.first() {
        for sub in ["um", "shared", "ucrt"] {
            let p = v.join(sub);
            if p.exists() {
                out.push(p);
            }
        }
    }
    out
}
