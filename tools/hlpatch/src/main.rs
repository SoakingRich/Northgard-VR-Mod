//! hlpatch: bytecode tooling for the Northgard VR mod.
//!
//! Phase 0 commands:
//!   hlpatch roundtrip <in.dat> <out.dat>   parse + re-serialize; output must be byte-identical
//!   hlpatch find <substring> <in.dat>      list functions whose name contains <substring>
//!
//!   hlpatch patch --smoke <hlboot.dat> <vrboot.dat>
//!                                          Phase 0b: call openxr@xr_smoke() every frame
//!   hlpatch patch --camtest <hlboot.dat> <vrboot.dat>
//!                                          Phase 0c: smoke + camera override hooks
//!
//! The original hlboot.dat is never modified; the game is launched with
//! `Northgard.exe vrboot.dat`.

use std::{
    fs::File,
    io::{BufReader, BufWriter, Read},
    process::ExitCode,
};

use hlbc::opcodes::Opcode;
use hlbc::types::{Native, Reg, RefField, RefFun, RefString, RefType, Type, TypeFun, ValBool};
use hlbc::{Bytecode, Str};

mod inject;

/// Index of a string in the constant pool, appending it if missing.
fn intern(code: &mut Bytecode, s: &str) -> RefString {
    if let Some(i) = code.strings.iter().position(|x| x.as_str() == s) {
        return RefString(i);
    }
    code.strings.push(Str::from(s.to_string()));
    RefString(code.strings.len() - 1)
}

/// Index of a type, appending it if missing.
fn intern_type(code: &mut Bytecode, t: Type) -> RefType {
    if let Some(i) = code.types.iter().position(|x| *x == t) {
        return RefType(i);
    }
    code.types.push(t);
    RefType(code.types.len() - 1)
}

/// Native library name for the mod's natives (HashLink loads `<name>.hdll`).
/// Builds use a versioned name (XR_LIB=openxr_N) so a new DLL never has to
/// overwrite one that a running game has locked.
fn xr_lib() -> String {
    std::env::var("XR_LIB").unwrap_or_else(|_| "openxr".to_string())
}

/// Declare a new HL native `lib@name` and return its findex.
fn add_native(code: &mut Bytecode, lib: &str, name: &str, args: Vec<RefType>, ret: RefType) -> RefFun {
    let t = intern_type(code, Type::Fun(TypeFun { args, ret }));
    let lib = intern(code, lib);
    let name = intern(code, name);
    let findex = RefFun(code.natives.len() + code.functions.len());
    code.natives.push(Native { name, lib, t, findex });
    findex
}

/// Name of an object type (e.g. "hxd.$System").
fn type_name(code: &Bytecode, t: RefType) -> Option<String> {
    code.types.get(t.0)?.get_type_obj().map(|o| code.strings[o.name.0].to_string())
}

/// Position in `functions` of the method `fname` whose parent type is `tname`.
fn find_method(code: &Bytecode, tname: &str, fname: &str) -> usize {
    let hits: Vec<usize> = code
        .functions
        .iter()
        .enumerate()
        .filter(|(_, f)| {
            code.strings[f.name.0].as_str() == fname
                && f.parent.and_then(|p| type_name(code, p)).as_deref() == Some(tname)
        })
        .map(|(i, _)| i)
        .collect();
    assert!(hits.len() == 1, "expected exactly one {tname}.{fname}, found {}", hits.len());
    hits[0]
}

/// Append a register of the given type to a function, returning it.
fn new_reg(code: &mut Bytecode, fpos: usize, t: RefType) -> Reg {
    let f = &mut code.functions[fpos];
    f.regs.push(t);
    Reg((f.regs.len() - 1) as u32)
}

fn type_index(code: &Bytecode, t: &Type) -> RefType {
    RefType(code.types.iter().position(|x| x == t).expect("type not found"))
}

/// Phase 0b: openxr@xr_smoke() at the start of every frame.
fn patch_smoke(code: &mut Bytecode) {
    let void_t = type_index(code, &Type::Void);
    let smoke = add_native(code, &xr_lib(), "xr_smoke", vec![], void_t);
    let main_loop = find_method(code, "hxd.$System", "mainLoop");
    let v = new_reg(code, main_loop, void_t);
    inject::insert_ops(&mut code.functions[main_loop], vec![(0, vec![Opcode::Call0 { dst: v, fun: smoke }])]);
}

/// Phase 0c: let the native lib re-aim the game camera (desktop test).
///
/// Identifying the main camera: a new global `vr_main_camera` (type
/// h3d.Camera) is added. Game.onUpdate stores `this.s3d.camera` in it right
/// before its own `camera.update()` call. h3d.Camera.update compares `this`
/// with that global (plain object-equality jump) so only the main camera ever
/// reaches the native side. No ToDyn is used: in this HashLink build ToDyn
/// boxes objects, so pointer identity on the native side is meaningless.
///
/// h3d.Camera.update prologue (for the main camera and the render camera):
///   if (xr_cam_active()) {
///     xr_cam_compute(pos.xyz, target.xyz, up.xyz, fovY, screenRatio, zoom, viewX, viewY);
///     pos.x = xr_out_next(); ... viewY = xr_out_next();   // same order
///   }
/// before every return:
///   if (xr_cam_restore()) { pos.x = xr_saved_next(); ... }
/// All field access is in bytecode using the game's own field indices (the
/// runtime's by-name lookups throw on some fields in this build); the native
/// side only does math. Gameplay state is never altered: pos is restored.
fn patch_camtest(code: &mut Bytecode) -> hlbc::types::RefGlobal {
    let void_t = type_index(code, &Type::Void);
    let bool_t = type_index(code, &Type::Bool);
    let f64_t = type_index(code, &Type::F64);
    let cam_active = add_native(code, &xr_lib(), "xr_cam_active", vec![], bool_t);
    // xr_cam_compute(14 values) / xr_out_next() / xr_saved_next(): the native
    // side receives the camera state, then hands back the values to use (and
    // later the originals to restore) one at a time in the same order:
    // pos.xyz, target.xyz, up.xyz, fovY, screenRatio, zoom, viewX, viewY.
    let cam_compute = add_native(code, &xr_lib(), "xr_cam_compute", vec![f64_t; 14], void_t);
    let out_next = add_native(code, &xr_lib(), "xr_out_next", vec![], f64_t);
    let cam_restore = add_native(code, &xr_lib(), "xr_cam_restore", vec![], bool_t);
    let saved_next = add_native(code, &xr_lib(), "xr_saved_next", vec![], f64_t);

    // New global holding the main camera (globals start out null).
    let (_, cam_t) = field_index(code, "h3d.scene.Scene", "camera");
    code.globals.push(cam_t);
    let main_g = hlbc::types::RefGlobal(code.globals.len() - 1);
    // Second global: the render camera that RenderContext.setCamera() fills with
    // `load(mainCamera)`. The scene is drawn with that copy, so it needs the
    // override too (it rebuilds its matrices from the restored main-camera pos).
    code.globals.push(cam_t);
    let render_g = hlbc::types::RefGlobal(code.globals.len() - 1);

    // Game.onUpdate: before `Call1 update@h3d.Camera.update(cam)` where cam came
    // from `Field s3d.camera`, insert `vr_main_camera = cam`.
    let cam_update = find_method(code, "h3d.Camera", "update");
    let cam_update_findex = code.functions[cam_update].findex;
    let (camera_f, _) = field_index(code, "h3d.scene.Scene", "camera");
    let gu = find_method(code, "Game", "onUpdate");
    let site = {
        let ops = &code.functions[gu].ops;
        let hits: Vec<(usize, Reg)> = ops
            .iter()
            .enumerate()
            .filter_map(|(i, op)| match op {
                Opcode::Call1 { fun, arg0, .. } if *fun == cam_update_findex => Some((i, *arg0)),
                _ => None,
            })
            .filter(|(i, r)| {
                // the camera register must have been loaded by `Field x.camera` shortly before
                ops[i.saturating_sub(4)..*i].iter().any(|o| {
                    matches!(o, Opcode::Field { dst, field, .. } if dst == r && *field == camera_f)
                })
            })
            .collect();
        assert!(hits.len() == 1, "expected one s3d.camera.update() in Game.onUpdate, found {}", hits.len());
        hits[0]
    };
    inject::insert_ops(
        &mut code.functions[gu],
        vec![(site.0, vec![Opcode::SetGlobal { global: main_g, src: site.1 }])],
    );

    // Diagnostics: count entries into candidate functions (xr_ping_0..3).
    let pings = [
        ("Game", "onUpdate"),
        ("Game", "render"),
        ("cam.CameraController", "update"),
        ("h3d.scene.Scene", "render"),
    ];
    for (k, (t, f)) in pings.iter().enumerate() {
        let nat = add_native(code, &xr_lib(), &format!("xr_ping_{k}"), vec![], void_t);
        let fp = find_method(code, t, f);
        let vr = new_reg(code, fp, void_t);
        inject::insert_ops(&mut code.functions[fp], vec![(0, vec![Opcode::Call0 { dst: vr, fun: nat }])]);
    }

    // h3d.Camera.load(src) prologue: if (src == main) render = this;
    {
        let ld = find_method(code, "h3d.Camera", "load");
        let m = new_reg(code, ld, cam_t);
        inject::insert_ops(
            &mut code.functions[ld],
            vec![(
                0,
                vec![
                    Opcode::GetGlobal { dst: m, global: main_g },
                    Opcode::JNotEq { a: Reg(1), b: m, offset: 1 },
                    Opcode::SetGlobal { global: render_g, src: Reg(0) },
                ],
            )],
        );
    }

    // h3d.Camera.update hooks.
    let (pos_f, vec_t) = field_index(code, "h3d.Camera", "pos");
    let (target_f, _) = field_index(code, "h3d.Camera", "target");
    let (up_f, _) = field_index(code, "h3d.Camera", "up");
    let scalars: Vec<RefField> = ["fovY", "screenRatio", "zoom", "viewX", "viewY"]
        .iter()
        .map(|n| field_index(code, "h3d.Camera", n).0)
        .collect();
    let xyz = [
        field_index(code, "h3d.VectorImpl", "x").0,
        field_index(code, "h3d.VectorImpl", "y").0,
        field_index(code, "h3d.VectorImpl", "z").0,
    ];
    let up = cam_update;
    let mc = new_reg(code, up, cam_t);
    let b = new_reg(code, up, bool_t);
    let vecs = [new_reg(code, up, vec_t), new_reg(code, up, vec_t), new_reg(code, up, vec_t)];
    let vals: Vec<Reg> = (0..14).map(|_| new_reg(code, up, f64_t)).collect();
    let n = new_reg(code, up, f64_t);
    let v = new_reg(code, up, void_t);
    let vec_fields = [pos_f, target_f, up_f];

    // Builds: if ((this == main || this == render) && test()) { body }
    //   0 GetGlobal mc = main; 1 JEq this,mc -> 4; 2 GetGlobal mc = render;
    //   3 JNotEq this,mc -> end; 4 Call0 b = test; 5 JFalse b -> end; 6.. body
    let guarded = |test: RefFun, body: Vec<Opcode>| -> Vec<Opcode> {
        let len = body.len() as i32;
        let mut ops = vec![
            Opcode::GetGlobal { dst: mc, global: main_g },
            Opcode::JEq { a: Reg(0), b: mc, offset: 2 },
            Opcode::GetGlobal { dst: mc, global: render_g },
            Opcode::JNotEq { a: Reg(0), b: mc, offset: len + 2 },
            Opcode::Call0 { dst: b, fun: test },
            Opcode::JFalse { cond: b, offset: len },
        ];
        ops.extend(body);
        ops
    };

    // "set everything from an iterator native" (used for override and restore)
    let write_all = |next: RefFun| -> Vec<Opcode> {
        let mut ops = vec![];
        for (vi, vf) in vec_fields.iter().enumerate() {
            ops.push(Opcode::GetThis { dst: vecs[vi], field: *vf });
            for k in 0..3 {
                ops.push(Opcode::Call0 { dst: n, fun: next });
                ops.push(Opcode::SetField { obj: vecs[vi], field: xyz[k], src: n });
            }
        }
        for f in scalars.iter() {
            ops.push(Opcode::Call0 { dst: n, fun: next });
            ops.push(Opcode::SetThis { field: *f, src: n });
        }
        ops
    };

    let mut pre_body = vec![];
    for (vi, vf) in vec_fields.iter().enumerate() {
        pre_body.push(Opcode::GetThis { dst: vecs[vi], field: *vf });
        for k in 0..3 {
            pre_body.push(Opcode::Field { dst: vals[vi * 3 + k], obj: vecs[vi], field: xyz[k] });
        }
    }
    for (i, f) in scalars.iter().enumerate() {
        pre_body.push(Opcode::GetThis { dst: vals[9 + i], field: *f });
    }
    pre_body.push(Opcode::CallN { dst: v, fun: cam_compute, args: vals.clone() });
    pre_body.extend(write_all(out_next));
    let post_body = write_all(saved_next);
    let pre = guarded(cam_active, pre_body);
    let post = guarded(cam_restore, post_body);

    // Tell the native side when this update is for the main camera (vs the
    // render camera): GetGlobal mc = main; JNotEq this,mc -> +1; xr_mark_main()
    let mark_main = add_native(code, &xr_lib(), "xr_mark_main", vec![], void_t);
    let mut pre_all = vec![
        Opcode::GetGlobal { dst: mc, global: main_g },
        Opcode::JNotEq { a: Reg(0), b: mc, offset: 1 },
        Opcode::Call0 { dst: v, fun: mark_main },
    ];
    pre_all.extend(pre);
    let mut ins = vec![(0, pre_all)];
    for r in inject::ret_indices(&code.functions[up]) {
        ins.push((r, post.clone()));
    }
    inject::insert_ops(&mut code.functions[up], ins);
    main_g
}

/// Phase 1: OpenXR theater mode. In hxd.System.mainLoop, right before the
/// game's `driver.present()` (the only CallMethod on an h3d.impl.Driver), insert
///     xr_frame(directx.get_back_buffer());
/// The native side copies the finished frame into an OpenXR swapchain and
/// releases the back-buffer reference that GetBuffer added.
fn patch_xr(code: &mut Bytecode, main_g: hlbc::types::RefGlobal) {
    let void_t = type_index(code, &Type::Void);
    let gbb = code
        .natives
        .iter()
        .find(|n| code.strings[n.lib.0].as_str() == "directx" && code.strings[n.name.0].as_str() == "get_back_buffer")
        .expect("directx@get_back_buffer native not found")
        .clone();
    let res_t = match &code.types[gbb.t.0] {
        Type::Fun(tf) => tf.ret,
        _ => panic!("get_back_buffer has an unexpected type"),
    };
    let xr_frame = add_native(code, &xr_lib(), "xr_frame", vec![res_t], void_t);

    // Phase 2: stereo. In hxd.App.mainLoop, before the game's
    //     engine.render(app)          (renders the whole frame)
    // insert
    //     if (xr_stereo_begin()) {    // waits on the headset, locates the eyes,
    //         engine.render(app);     // left eye (camera hook applies the eye)
    //         xr_eye_done(directx.get_back_buffer());  // copy to left image
    //     }
    // after which the original call renders the right eye, and xr_frame()
    // before present copies it and submits both eyes.
    let bool_t = type_index(code, &Type::Bool);
    let stereo_begin = add_native(code, &xr_lib(), "xr_stereo_begin", vec![], bool_t);
    let eye_done = add_native(code, &xr_lib(), "xr_eye_done", vec![res_t], void_t);
    {
        let eng_render = code.functions[find_method(code, "h3d.Engine", "render")].findex;
        let am = find_method(code, "hxd.App", "mainLoop");
        let (site, eng_reg, app_reg) = {
            let f = &code.functions[am];
            let hits: Vec<(usize, Reg, Reg)> = f
                .ops
                .iter()
                .enumerate()
                .filter_map(|(i, op)| match op {
                    Opcode::Call2 { fun, arg0, arg1, .. } if *fun == eng_render => Some((i, *arg0, *arg1)),
                    _ => None,
                })
                .collect();
            assert!(hits.len() == 1, "expected one engine.render(app) in hxd.App.mainLoop, found {}", hits.len());
            hits[0]
        };
        let b = new_reg(code, am, bool_t);
        let bb = new_reg(code, am, res_t);
        let v = new_reg(code, am, void_t);
        inject::insert_ops(
            &mut code.functions[am],
            vec![(
                site,
                vec![
                    Opcode::Call0 { dst: b, fun: stereo_begin },
                    Opcode::JFalse { cond: b, offset: 3 },
                    Opcode::Call2 { dst: b, fun: eng_render, arg0: eng_reg, arg1: app_reg },
                    Opcode::Call0 { dst: bb, fun: gbb.findex },
                    Opcode::Call1 { dst: v, fun: eye_done, arg0: bb },
                ],
            )],
        );
    }

    // Phase 2b: UI on its own floating panel.
    //  * Game.onRender draws underScene (2D backdrop), s3d (world), s2d (UI).
    //    Guard them: eyes draw underScene + s3d only (xr_draw_2d() false),
    //    the UI pass draws s2d only (xr_draw_3d() false).
    //  * hxd.App.mainLoop, after the game's own engine.render (right eye):
    //        xr_eye_done(get_back_buffer());            // copy the right eye
    //        if (xr_ui_begin(get_back_buffer()))        // clears to transparent
    //            engine.render(app);                    // UI-only pass
    //    xr_frame() before present then copies the UI into a quad layer and
    //    submits eyes + UI.
    let draw_3d = add_native(code, &xr_lib(), "xr_draw_3d", vec![], bool_t);
    let draw_2d = add_native(code, &xr_lib(), "xr_draw_2d", vec![], bool_t);
    let ui_begin = add_native(code, &xr_lib(), "xr_ui_begin", vec![res_t], bool_t);
    {
        let h2d_render = code.functions[find_method(code, "h2d.Scene", "render")].findex;
        let h3d_render = code.functions[find_method(code, "h3d.scene.Scene", "render")].findex;
        let gr = find_method(code, "Game", "onRender");
        let calls: Vec<(usize, RefFun)> = code.functions[gr]
            .ops
            .iter()
            .enumerate()
            .filter_map(|(i, op)| match op {
                Opcode::Call2 { fun, .. } if *fun == h2d_render || *fun == h3d_render => Some((i, *fun)),
                _ => None,
            })
            .collect();
        // expected order: underScene (2D), s3d (3D), s2d (2D UI)
        assert!(
            calls.len() == 3 && calls[0].1 == h2d_render && calls[1].1 == h3d_render && calls[2].1 == h2d_render,
            "Game.onRender has an unexpected shape: {:?}",
            calls
        );
        let b = new_reg(code, gr, bool_t);
        let old_vis = new_reg(code, gr, bool_t);
        let guard = |test: RefFun| vec![Opcode::Call0 { dst: b, fun: test }, Opcode::JFalse { cond: b, offset: 1 }];
        // The UI must not be *skipped* in the eye passes: AppBase.render's final
        // full-screen composite uses s2d's RenderContext, which s2d.render sets
        // up. Instead hide it: old = s2d.visible; if (!xr_draw_2d()) s2d.visible
        // = false; s2d.render(e); s2d.visible = old;
        let (vis_f, _) = field_index(code, "h2d.Scene", "visible");
        let s2d_reg = match &code.functions[gr].ops[calls[2].0] {
            Opcode::Call2 { arg0, .. } => *arg0,
            _ => unreachable!(),
        };
        let hide = vec![
            Opcode::Field { dst: old_vis, obj: s2d_reg, field: vis_f },
            Opcode::Call0 { dst: b, fun: draw_2d },
            Opcode::JTrue { cond: b, offset: 1 },
            Opcode::SetField { obj: s2d_reg, field: vis_f, src: b },
        ];
        let restore = vec![Opcode::SetField { obj: s2d_reg, field: vis_f, src: old_vis }];
        inject::insert_ops(
            &mut code.functions[gr],
            vec![
                (calls[0].0, guard(draw_3d)),
                (calls[1].0, guard(draw_3d)),
                (calls[2].0, hide),
                (calls[2].0 + 1, restore),
            ],
        );
    }
    {
        let eng_render = code.functions[find_method(code, "h3d.Engine", "render")].findex;
        let am = find_method(code, "hxd.App", "mainLoop");
        // the game's own engine.render call is now the *last* Call2 to Engine.render
        let (site, eng_reg, app_reg) = code.functions[am]
            .ops
            .iter()
            .enumerate()
            .filter_map(|(i, op)| match op {
                Opcode::Call2 { fun, arg0, arg1, .. } if *fun == eng_render => Some((i, *arg0, *arg1)),
                _ => None,
            })
            .last()
            .unwrap();
        let b = new_reg(code, am, bool_t);
        let bb = new_reg(code, am, res_t);
        let v = new_reg(code, am, void_t);
        let cam_t = code.globals[main_g.0];
        let mcam = new_reg(code, am, cam_t);
        let cam_update_f = code.functions[find_method(code, "h3d.Camera", "update")].findex;
        inject::insert_ops(
            &mut code.functions[am],
            vec![(
                site + 1,
                vec![
                    Opcode::Call0 { dst: bb, fun: gbb.findex },
                    Opcode::Call1 { dst: v, fun: eye_done, arg0: bb },
                    Opcode::Call0 { dst: bb, fun: gbb.findex },
                    Opcode::Call1 { dst: b, fun: ui_begin, arg0: bb },
                    Opcode::JFalse { cond: b, offset: 1 },
                    Opcode::Call2 { dst: b, fun: eng_render, arg0: eng_reg, arg1: app_reg },
                    // Re-update the main camera after rendering so its matrices
                    // (used by next frame's mouse picking) are the pointer camera,
                    // not the last eye: if (vr_main_camera != null) update().
                    Opcode::GetGlobal { dst: mcam, global: main_g },
                    Opcode::JNull { reg: mcam, offset: 1 },
                    Opcode::Call1 { dst: v, fun: cam_update_f, arg0: mcam },
                ],
            )],
        );
    }

    let ml = find_method(code, "hxd.$System", "mainLoop");
    let site = {
        let f = &code.functions[ml];
        let hits: Vec<usize> = f
            .ops
            .iter()
            .enumerate()
            .filter(|(_, op)| match op {
                Opcode::CallMethod { args, .. } => type_name(code, f.regs[args[0].0 as usize]).as_deref() == Some("h3d.impl.Driver"),
                _ => false,
            })
            .map(|(i, _)| i)
            .collect();
        assert!(hits.len() == 1, "expected one driver.present() in hxd.System.mainLoop, found {}", hits.len());
        hits[0]
    };
    let bb = new_reg(code, ml, res_t);
    let v = new_reg(code, ml, void_t);
    inject::insert_ops(
        &mut code.functions[ml],
        vec![(
            site,
            vec![
                Opcode::Call0 { dst: bb, fun: gbb.findex },
                Opcode::Call1 { dst: v, fun: xr_frame, arg0: bb },
            ],
        )],
    );
}

/// Phase 6: render size independent of the window.
///
/// While VR runs the engine must render the eyes at the headset's recommended
/// size, not the (smaller) desktop window's. Heaps sizes the engine from
/// hxd.Window.get_width/get_height, so:
///   get_width/get_height prologue:  if (xr_rt_on()) return xr_rt_w() / xr_rt_h();
/// Present then stretches the larger back buffer into the window.
/// Mouse events arrive in window pixels, so hxd.Window.onEvent rescales them:
///   e.mouseX = xr_mouse_x(e.mouseX); e.mouseY = xr_mouse_y(e.mouseY);
/// And before driver.present(): if (xr_vsync_off()) getInstance().set_vsync(false)
/// so the monitor's refresh rate doesn't cap the headset.
fn patch_rt(code: &mut Bytecode) {
    let bool_t = type_index(code, &Type::Bool);
    let i32_t = type_index(code, &Type::I32);
    let void_t = type_index(code, &Type::Void);
    let rt_on = add_native(code, &xr_lib(), "xr_rt_on", vec![], bool_t);
    let rt_w = add_native(code, &xr_lib(), "xr_rt_w", vec![], i32_t);
    let rt_h = add_native(code, &xr_lib(), "xr_rt_h", vec![], i32_t);
    let mouse_x = add_native(code, &xr_lib(), "xr_mouse_x", vec![i32_t], i32_t);
    let mouse_y = add_native(code, &xr_lib(), "xr_mouse_y", vec![i32_t], i32_t);
    let vsync_off = add_native(code, &xr_lib(), "xr_vsync_off", vec![], bool_t);

    for (name, nat) in [("get_width", rt_w), ("get_height", rt_h)] {
        let fp = find_method(code, "hxd.Window", name);
        let b = new_reg(code, fp, bool_t);
        let r = new_reg(code, fp, i32_t);
        inject::insert_ops(
            &mut code.functions[fp],
            vec![(
                0,
                vec![
                    Opcode::Call0 { dst: b, fun: rt_on },
                    Opcode::JFalse { cond: b, offset: 2 },
                    Opcode::Call0 { dst: r, fun: nat },
                    Opcode::Ret { ret: r },
                ],
            )],
        );
    }

    {
        let fp = find_method(code, "hxd.Window", "onEvent");
        let (fx, _) = field_index(code, "dx.Event", "mouseX");
        let (fy, _) = field_index(code, "dx.Event", "mouseY");
        let e = Reg(1); // onEvent(this, e)
        let r = new_reg(code, fp, i32_t);
        inject::insert_ops(
            &mut code.functions[fp],
            vec![(
                0,
                vec![
                    Opcode::Field { dst: r, obj: e, field: fx },
                    Opcode::Call1 { dst: r, fun: mouse_x, arg0: r },
                    Opcode::SetField { obj: e, field: fx, src: r },
                    Opcode::Field { dst: r, obj: e, field: fy },
                    Opcode::Call1 { dst: r, fun: mouse_y, arg0: r },
                    Opcode::SetField { obj: e, field: fy, src: r },
                ],
            )],
        );
    }

    {
        let get_inst = code.functions[find_method(code, "hxd.$Window", "getInstance")].findex;
        let set_vsync = code.functions[find_method(code, "hxd.Window", "set_vsync")].findex;
        let win_t = code.functions[find_method(code, "hxd.Window", "set_vsync")].regs[0];
        let ml = find_method(code, "hxd.$System", "mainLoop");
        let site = {
            let f = &code.functions[ml];
            f.ops
                .iter()
                .enumerate()
                .filter(|(_, op)| match op {
                    Opcode::CallMethod { args, .. } => {
                        type_name(code, f.regs[args[0].0 as usize]).as_deref() == Some("h3d.impl.Driver")
                    }
                    _ => false,
                })
                .map(|(i, _)| i)
                .last()
                .expect("driver.present() not found")
        };
        let b = new_reg(code, ml, bool_t);
        let w = new_reg(code, ml, win_t);
        let v = new_reg(code, ml, void_t);
        let _ = v;
        inject::insert_ops(
            &mut code.functions[ml],
            vec![(
                site,
                vec![
                    Opcode::Call0 { dst: b, fun: vsync_off },
                    Opcode::JFalse { cond: b, offset: 3 },
                    Opcode::Call0 { dst: w, fun: get_inst },
                    Opcode::Bool { dst: b, value: ValBool(false) },
                    Opcode::Call2 { dst: b, fun: set_vsync, arg0: w, arg1: b },
                ],
            )],
        );
    }
}

/// Phase 4: UI with alpha. AppBase.render draws s2d into an offscreen target
/// that is cleared to transparent, then composites it opaquely onto the back
/// buffer. Right after popTarget, hand that target to the native side:
///   if (xr_ui_capture()) xr_ui_target(tex.t.res);
/// (xr_ui_capture() is true only in the UI pass.)
fn patch_ui_alpha(code: &mut Bytecode) {
    let bool_t = type_index(code, &Type::Bool);
    let void_t = type_index(code, &Type::Void);
    let fp = find_method(code, "AppBase", "render");
    let pop = code.functions[find_method(code, "h3d.Engine", "popTarget")].findex;
    let alloc = code.functions[find_method(code, "h3d.impl.TextureCache", "allocTarget")].findex;
    let (pop_site, tex_reg) = {
        let f = &code.functions[fp];
        let tex = f.ops.iter().find_map(|op| match op {
            Opcode::CallN { dst, fun, .. } if *fun == alloc => Some(*dst),
            _ => None,
        });
        let site = f.ops.iter().position(|op| matches!(op, Opcode::Call1 { fun, .. } if *fun == pop));
        (site.expect("popTarget not found in AppBase.render"), tex.expect("allocTarget not found"))
    };
    let (tfield, vt) = field_index(code, "h3d.mat.Texture", "t");
    let (res_field, res_t) = match &code.types[vt.0] {
        Type::Virtual { fields } => {
            let i = fields.iter().position(|f| code.strings[f.name.0].as_str() == "res").expect("t.res");
            (RefField(i), fields[i].t)
        }
        _ => panic!("Texture.t is not a virtual"),
    };
    let capture = add_native(code, &xr_lib(), "xr_ui_capture", vec![], bool_t);
    let target = add_native(code, &xr_lib(), "xr_ui_target", vec![res_t], void_t);
    let b = new_reg(code, fp, bool_t);
    let tv = new_reg(code, fp, vt);
    let r = new_reg(code, fp, res_t);
    let v = new_reg(code, fp, void_t);
    inject::insert_ops(
        &mut code.functions[fp],
        vec![(
            pop_site + 1,
            vec![
                Opcode::Call0 { dst: b, fun: capture },
                Opcode::JFalse { cond: b, offset: 4 },
                Opcode::Field { dst: tv, obj: tex_reg, field: tfield },
                Opcode::JNull { reg: tv, offset: 2 },
                Opcode::Field { dst: r, obj: tv, field: res_field },
                Opcode::Call1 { dst: v, fun: target, arg0: r },
            ],
        )],
    );
}

/// HUD layout from the game itself: at the start of ui.BaseUI.update, hand the
/// BaseUI object to xr_ui_windows, which reads every ui.Window's class name and
/// on-screen rect (root absX/absY, calculatedWidth/Height, scale) for the VR panels.
fn patch_ui_windows(code: &mut Bytecode) {
    let void_t = type_index(code, &Type::Void);
    let dyn_t = type_index(code, &Type::Dyn);
    let fp = find_method(code, "ui.BaseUI", "update");
    let nat = add_native(code, &xr_lib(), "xr_ui_windows", vec![dyn_t], void_t);
    let d = new_reg(code, fp, dyn_t);
    let v = new_reg(code, fp, void_t);
    inject::insert_ops(
        &mut code.functions[fp],
        vec![(0, vec![Opcode::UnsafeCast { dst: d, src: Reg(0) }, Opcode::Call1 { dst: v, fun: nat, arg0: d }])],
    );
}

/// Index of a named field (inherited fields included) in an object type,
/// as used by Field/SetField/GetThis.
fn field_index(code: &Bytecode, tname: &str, fname: &str) -> (RefField, RefType) {
    for t in code.types.iter() {
        if let Some(o) = t.get_type_obj() {
            if code.strings[o.name.0].as_str() == tname {
                for (i, f) in o.fields.iter().enumerate() {
                    if code.strings[f.name.0].as_str() == fname {
                        return (RefField(i), f.t);
                    }
                }
            }
        }
    }
    panic!("field {tname}.{fname} not found");
}

fn save(code: &Bytecode, path: &str) {
    let mut w = BufWriter::new(File::create(path).expect("create output"));
    code.serialize(&mut w).expect("serialize");
}

fn load(path: &str) -> Bytecode {
    let f = File::open(path).unwrap_or_else(|e| panic!("open {path}: {e}"));
    Bytecode::deserialize(&mut BufReader::new(f)).unwrap_or_else(|e| panic!("parse {path}: {e}"))
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    match args.get(1).map(String::as_str) {
        Some("roundtrip") if args.len() == 4 => {
            let code = load(&args[2]);
            {
                let mut w = BufWriter::new(File::create(&args[3]).expect("create output"));
                code.serialize(&mut w).expect("serialize");
            }
            let mut a = Vec::new();
            let mut b = Vec::new();
            File::open(&args[2]).unwrap().read_to_end(&mut a).unwrap();
            File::open(&args[3]).unwrap().read_to_end(&mut b).unwrap();
            if a == b {
                println!("OK: byte-identical ({} bytes)", a.len());
                ExitCode::SUCCESS
            } else {
                eprintln!("MISMATCH: {} vs {} bytes", a.len(), b.len());
                ExitCode::FAILURE
            }
        }
        Some("find") if args.len() == 4 => {
            let code = load(&args[3]);
            for f in code.functions.iter() {
                let name = f.name(&code).to_string();
                if name.contains(&args[2]) {
                    println!("{}@{}", name, f.findex.0);
                }
            }
            ExitCode::SUCCESS
        }
        Some("patch") if args.len() == 5 && ["--smoke", "--camtest", "--phase1"].contains(&args[2].as_str()) => {
            let mut code = load(&args[3]);
            let before = code.natives.len();
            patch_smoke(&mut code);
            let mut main_g = None;
            if args[2] == "--camtest" || args[2] == "--phase1" {
                main_g = Some(patch_camtest(&mut code));
            }
            if args[2] == "--phase1" {
                patch_xr(&mut code, main_g.unwrap());
                patch_rt(&mut code);
                patch_ui_alpha(&mut code);
                patch_ui_windows(&mut code);
            }
            save(&code, &args[4]);
            let check = load(&args[4]); // verify the output parses again
            println!("OK: wrote {} (natives {} -> {})", args[4], before, check.natives.len());
            ExitCode::SUCCESS
        }
        Some("rawops") if args.len() == 5 => {
            // Debug-print raw opcodes: hlpatch rawops <findex> <in.dat>
            let code = load(&args[3]);
            let fi: usize = args[2].parse().unwrap();
            let f = code.functions.iter().find(|f| f.findex.0 == fi).expect("no such findex");
            let lim: usize = args[4].parse().unwrap_or(usize::MAX);
            for (i, op) in f.ops.iter().enumerate().take(lim) {
                println!("{i:4}: {op:?}");
            }
            ExitCode::SUCCESS
        }
        Some("static-writes") if args.len() == 5 => {
            // Find writes to a static field: hlpatch static-writes <global> <field> <in.dat>
            let code = load(&args[4]);
            let gi: usize = args[2].parse().unwrap();
            let fi: usize = args[3].parse().unwrap();
            for f in code.functions.iter() {
                let mut gregs = std::collections::HashSet::new();
                for (i, op) in f.ops.iter().enumerate() {
                    match op {
                        Opcode::GetGlobal { dst, global } if global.0 == gi => { gregs.insert(dst.0); }
                        Opcode::SetField { obj, field, src } if field.0 == fi && gregs.contains(&obj.0) => {
                            println!("{}@{} op {i}: src reg{} : {}", f.name(&code), f.findex.0, src.0,
                                f.regs[src.0 as usize].display::<hlbc::fmt::EnhancedFmt>(&code));
                        }
                        _ => {}
                    }
                }
            }
            ExitCode::SUCCESS
        }
        Some("types") if args.len() == 4 => {
            // Object types whose name contains a substring, with their super type.
            let code = load(&args[3]);
            for (i, t) in code.types.iter().enumerate() {
                if let Some(o) = t.get_type_obj() {
                    let n = code.strings[o.name.0].as_str();
                    if n.contains(&args[2]) {
                        let sup = o.super_.and_then(|s| type_name(&code, s)).unwrap_or_default();
                        println!("{i:6}  {n}  : {sup}  ({} fields, {} methods)", o.fields.len(), o.protos.len());
                    }
                }
            }
            ExitCode::SUCCESS
        }
        Some("methods") if args.len() == 4 => {
            // Functions whose parent type is exactly <TypeName>.
            let code = load(&args[3]);
            for f in code.functions.iter() {
                if f.parent.and_then(|p| type_name(&code, p)).as_deref() == Some(args[2].as_str()) {
                    println!("{}@{}  {}", code.strings[f.name.0].as_str(), f.findex.0,
                        f.t.display::<hlbc::fmt::EnhancedFmt>(&code));
                }
            }
            ExitCode::SUCCESS
        }
        Some("callers") if args.len() == 4 => {
            // Functions that call findex <n> directly.
            let code = load(&args[3]);
            let n: usize = args[2].parse().unwrap();
            for f in code.functions.iter() {
                let hit = f.ops.iter().any(|op| match op {
                    Opcode::Call0 { fun, .. } | Opcode::Call1 { fun, .. } | Opcode::Call2 { fun, .. }
                    | Opcode::Call3 { fun, .. } | Opcode::Call4 { fun, .. } | Opcode::CallN { fun, .. } => fun.0 == n,
                    Opcode::StaticClosure { fun, .. } | Opcode::InstanceClosure { fun, .. } => fun.0 == n,
                    _ => false,
                });
                if hit {
                    let parent = f.parent.and_then(|p| type_name(&code, p)).unwrap_or_default();
                    println!("{}.{}@{}", parent, code.strings[f.name.0].as_str(), f.findex.0);
                }
            }
            ExitCode::SUCCESS
        }
        Some("strings") if args.len() == 4 => {
            let code = load(&args[3]);
            for (i, s) in code.strings.iter().enumerate() {
                if s.as_str().contains(&args[2]) { println!("{i:6}  {}", s.as_str()); }
            }
            ExitCode::SUCCESS
        }
        Some("fields") if args.len() == 4 => {
            // List an object type's fields (including inherited ones) with the
            // index used by Field/SetField/GetThis opcodes and the field type.
            let code = load(&args[3]);
            for t in code.types.iter() {
                if let Some(o) = t.get_type_obj() {
                    if code.strings[o.name.0].as_str() == args[2] {
                        for (i, f) in o.fields.iter().enumerate() {
                            println!("{:3}  {:24} {}", i, code.strings[f.name.0].as_str(), f.t.display::<hlbc::fmt::EnhancedFmt>(&code));
                        }
                    }
                }
            }
            ExitCode::SUCCESS
        }
        _ => {
            eprintln!("usage:\n  hlpatch roundtrip <in.dat> <out.dat>\n  hlpatch find <substring> <in.dat>\n  hlpatch fields <TypeName> <in.dat>\n  hlpatch patch --smoke|--camtest|--phase1 <hlboot.dat> <vrboot.dat>");
            ExitCode::FAILURE
        }
    }
}
