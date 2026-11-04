// Phase 1 OpenXR theater mode and Phase 2 stereo (see xr_theater.c).
#pragma once

// Controller state for one frame (positions in the play space, metres).
typedef struct {
    int floor_space;           // play space origin is on the floor (STAGE)
    int hand_valid[2];         // 0 = left, 1 = right
    float hand_pos[2][3];
    float hand_quat[2][4];     // grip orientation
    int hand_quat_valid[2];
    float grip[2], trigger[2];
    float stick[2][2];
    int btn_lower[2];          // X (left) / A (right)
    int btn_upper[2];          // Y (left) / B (right)
    int stick_click[2];
    int menu[2];               // left: menu button (right is the system button, not readable)
    int trigger_touch[2];
    int aim_valid[2];          // aim pose: pointing ray origin/orientation
    float aim_pos[2][3];
    float aim_quat[2][4];
} xr_input;

// Implemented in openxr_hdll.c: diorama controls, once per stereo frame.
void dio_frame(const xr_input *in, const float head_pos[3], const float head_quat[4], double dt);
void xr_theater_frame(void *backbuffer);
int xrt_stereo_begin(void);
void xrt_eye_done(void *backbuffer);
int xr_current_eye(float pos[3], float quat[4], float tan_lrud[4]);
int xr_stereo_toggle(void);
int xrt_ui_begin(void *backbuffer);
int xrt_draw_3d(void);
int xrt_draw_2d(void);
int xr_ui_toggle(void);
void xrt_set_panel_anchor(const float head[3], float yaw);
void xr_window_mirror(int rec_w, int rec_h);
int xrt_stereo_on(void);
int xrt_rt_size(int *w, int *h);
// VR options menu: returns 1 when it used the right controller this frame (the game gets no mouse).
int xrt_options_input(const xr_input *in);
int xrt_running(void);
int xrt_ui_capture(void);
void xrt_ui_target(void *res);
int xrt_panel_hit(const float o[3], const float d[3], float *ux, float *uy, float *dist);
void xrt_haptic(int hand, float amplitude, int ms);
