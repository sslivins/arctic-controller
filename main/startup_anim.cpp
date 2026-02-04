/*
 * Arctic Heat Pump Controller - Startup Animation
 * Smooth Arctic-themed boot animation using smooth_ui_toolkit
 */
#include "startup_anim.h"
#include <lvgl.h>
#include <smooth_ui_toolkit.h>
#include <smooth_lvgl.h>
#include <memory>
#include <vector>
#include <cstdlib>
#include <cmath>

using namespace smooth_ui_toolkit;
using namespace smooth_ui_toolkit::lvgl_cpp;

// Colors
#define COLOR_BG_DARK       0x0a1628
#define COLOR_BG_GRADIENT   0x1a3a5c
#define COLOR_ARCTIC_TEXT   0x00d4ff
#define COLOR_ARCTIC_GLOW   0x0088aa
#define COLOR_HEAT_TEXT     0xff6b35
#define COLOR_PUMPS_TEXT    0xffffff
#define COLOR_PARTICLE      0xaaddff

// Number of snowflake particles
#define NUM_PARTICLES       15

// Animation state
enum class AnimState {
    Idle = 0,
    StartupDelay,
    ArcticSlide,
    HeatFade,
    PumpsFade,
    Hold,
    FadeOut,
    Complete
};

// Particle structure for snowflakes
struct Particle {
    std::unique_ptr<Container> obj;
    float x;
    float y;
    float speed;
    float drift_offset;
    int initial_opa;
};

// Animation context
static struct {
    bool running = false;
    AnimState state = AnimState::Idle;
    uint32_t state_start_time = 0;
    void (*on_complete)(void) = nullptr;
    
    // UI elements
    std::unique_ptr<Label> label_arctic;
    std::unique_ptr<Label> label_heat;
    std::unique_ptr<Label> label_pumps;
    
    // Smooth animations
    AnimateValue anim_arctic_x;
    AnimateValue anim_arctic_opa;
    AnimateValue anim_arctic_glow;
    AnimateValue anim_heat_opa;
    AnimateValue anim_heat_y;
    AnimateValue anim_pumps_opa;
    AnimateValue anim_pumps_y;
    AnimateValue anim_fadeout;
    
    // Particles
    std::vector<Particle> particles;
    
    // Screen dimensions
    int16_t screen_w = 0;
    int16_t screen_h = 0;
    
    // Calculated positions
    int arctic_center_x = 0;
    int heat_pumps_target_y = 0;
    
    // Glow animation state
    bool glow_increasing = true;
} ctx;

// Forward declarations
static void create_particles();
static void update_particles();
static void transition_state(AnimState new_state);
static void cleanup_animation();

bool startup_anim_init(void (*on_complete)(void))
{
    if (ctx.running) {
        return false;
    }
    
    // Initialize context
    ctx.running = true;
    ctx.state = AnimState::Idle;
    ctx.on_complete = on_complete;
    
    // Get screen
    lv_obj_t* scr = lv_scr_act();
    ctx.screen_w = lv_obj_get_width(scr);
    ctx.screen_h = lv_obj_get_height(scr);
    
    // Set dark blue gradient background
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG_DARK), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(COLOR_BG_GRADIENT), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    
    // Create particles first (behind text)
    create_particles();
    
    // Create "ARCTIC" label - use absolute positioning for animation
    ctx.label_arctic = std::make_unique<Label>(scr);
    ctx.label_arctic->setText("ARCTIC");
    ctx.label_arctic->setTextFont(&lv_font_montserrat_48);
    ctx.label_arctic->setTextColor(lv_color_hex(COLOR_ARCTIC_TEXT));
    lv_obj_set_style_text_letter_space(ctx.label_arctic->get(), 8, LV_PART_MAIN);
    // Glow effect using shadow
    ctx.label_arctic->setShadowColor(lv_color_hex(COLOR_ARCTIC_GLOW));
    ctx.label_arctic->setShadowWidth(30);
    ctx.label_arctic->setShadowSpread(5);
    ctx.label_arctic->setShadowOffsetX(0);
    ctx.label_arctic->setShadowOffsetY(0);
    // Force layout to get accurate dimensions
    lv_obj_update_layout(ctx.label_arctic->get());
    // Calculate and store center position for ARCTIC
    int arctic_w = lv_obj_get_width(ctx.label_arctic->get());
    ctx.arctic_center_x = (ctx.screen_w - arctic_w) / 2;
    int arctic_y = (ctx.screen_h - lv_obj_get_height(ctx.label_arctic->get())) / 2 - 60;
    ctx.label_arctic->setY(arctic_y);
    ctx.label_arctic->setX(-400);  // Start off-screen left
    ctx.label_arctic->setOpa(255);
    
    // Calculate center Y for "HEAT PUMPS" line and store target
    ctx.heat_pumps_target_y = ctx.screen_h / 2 + 20;
    int heat_pumps_start_y = ctx.heat_pumps_target_y + 40;  // Start 40px lower
    
    // Create "HEAT" label - positioned left of center
    ctx.label_heat = std::make_unique<Label>(scr);
    ctx.label_heat->setText("HEAT");
    ctx.label_heat->setTextFont(&lv_font_montserrat_40);
    ctx.label_heat->setTextColor(lv_color_hex(COLOR_HEAT_TEXT));
    lv_obj_set_style_text_letter_space(ctx.label_heat->get(), 4, LV_PART_MAIN);
    // Warm glow
    ctx.label_heat->setShadowColor(lv_color_hex(COLOR_HEAT_TEXT));
    ctx.label_heat->setShadowWidth(20);
    ctx.label_heat->setShadowSpread(3);
    lv_obj_update_layout(ctx.label_heat->get());
    int heat_w = lv_obj_get_width(ctx.label_heat->get());
    int heat_x = ctx.screen_w / 2 - heat_w - 10;  // Left of center with gap
    ctx.label_heat->setPos(heat_x, heat_pumps_start_y);
    ctx.label_heat->setOpa(0);  // Start invisible
    
    // Create "PUMPS" label - positioned right of center
    ctx.label_pumps = std::make_unique<Label>(scr);
    ctx.label_pumps->setText("PUMPS");
    ctx.label_pumps->setTextFont(&lv_font_montserrat_40);
    ctx.label_pumps->setTextColor(lv_color_hex(COLOR_PUMPS_TEXT));
    lv_obj_set_style_text_letter_space(ctx.label_pumps->get(), 4, LV_PART_MAIN);
    lv_obj_update_layout(ctx.label_pumps->get());
    int pumps_x = ctx.screen_w / 2 + 10;  // Right of center with gap
    ctx.label_pumps->setPos(pumps_x, heat_pumps_start_y);
    ctx.label_pumps->setOpa(0);  // Start invisible
    
    // Configure spring animations for smooth movement
    
    // ARCTIC slide animation - spring for bouncy effect
    ctx.anim_arctic_x.springOptions().visualDuration = 0.8f;
    ctx.anim_arctic_x.springOptions().bounce = 0.15f;
    ctx.anim_arctic_x.pause();
    ctx.anim_arctic_x.teleport(-400);
    
    // ARCTIC opacity
    ctx.anim_arctic_opa.easingOptions().duration = 0.3f;
    ctx.anim_arctic_opa.easingOptions().easingFunction = ease::ease_out_cubic;
    ctx.anim_arctic_opa.pause();
    ctx.anim_arctic_opa.teleport(255);
    
    // ARCTIC glow pulse
    ctx.anim_arctic_glow.springOptions().visualDuration = 1.0f;
    ctx.anim_arctic_glow.springOptions().bounce = 0.0f;
    ctx.anim_arctic_glow.pause();
    ctx.anim_arctic_glow.teleport(5);
    
    // HEAT fade + rise animation
    ctx.anim_heat_opa.easingOptions().duration = 0.5f;
    ctx.anim_heat_opa.easingOptions().easingFunction = ease::ease_out_cubic;
    ctx.anim_heat_opa.pause();
    ctx.anim_heat_opa.teleport(0);
    
    ctx.anim_heat_y.springOptions().visualDuration = 0.6f;
    ctx.anim_heat_y.springOptions().bounce = 0.1f;
    ctx.anim_heat_y.pause();
    ctx.anim_heat_y.teleport((float)(ctx.heat_pumps_target_y + 40));  // Start 40px lower
    
    // PUMPS fade + rise animation
    ctx.anim_pumps_opa.easingOptions().duration = 0.5f;
    ctx.anim_pumps_opa.easingOptions().easingFunction = ease::ease_out_cubic;
    ctx.anim_pumps_opa.pause();
    ctx.anim_pumps_opa.teleport(0);
    
    ctx.anim_pumps_y.springOptions().visualDuration = 0.6f;
    ctx.anim_pumps_y.springOptions().bounce = 0.1f;
    ctx.anim_pumps_y.pause();
    ctx.anim_pumps_y.teleport((float)(ctx.heat_pumps_target_y + 40));  // Start 40px lower
    
    // Fadeout animation
    ctx.anim_fadeout.easingOptions().duration = 0.4f;
    ctx.anim_fadeout.easingOptions().easingFunction = ease::ease_in_cubic;
    ctx.anim_fadeout.pause();
    ctx.anim_fadeout.teleport(255);
    
    // Start with a brief delay
    ctx.state_start_time = lv_tick_get();
    transition_state(AnimState::StartupDelay);
    
    return true;
}

bool startup_anim_update(void)
{
    if (!ctx.running) {
        return false;
    }
    
    uint32_t now = lv_tick_get();
    uint32_t elapsed = now - ctx.state_start_time;
    
    // Update particles
    update_particles();
    
    // Update animations and apply to objects
    ctx.anim_arctic_x.update();
    ctx.anim_arctic_opa.update();
    ctx.anim_arctic_glow.update();
    ctx.anim_heat_opa.update();
    ctx.anim_heat_y.update();
    ctx.anim_pumps_opa.update();
    ctx.anim_pumps_y.update();
    ctx.anim_fadeout.update();
    
    // Apply ARCTIC animations
    if (ctx.label_arctic) {
        if (ctx.state == AnimState::FadeOut) {
            ctx.label_arctic->setOpa((int)ctx.anim_fadeout.directValue());
        } else {
            ctx.label_arctic->setX((int)ctx.anim_arctic_x.directValue());
            
            // Pulsing glow effect
            if (ctx.state >= AnimState::ArcticSlide && ctx.state < AnimState::FadeOut) {
                int glow = (int)ctx.anim_arctic_glow.directValue();
                ctx.label_arctic->setShadowSpread(glow);
                
                // Oscillate glow
                if (ctx.anim_arctic_glow.done()) {
                    if (ctx.glow_increasing) {
                        ctx.anim_arctic_glow = 15;
                        ctx.glow_increasing = false;
                    } else {
                        ctx.anim_arctic_glow = 5;
                        ctx.glow_increasing = true;
                    }
                    ctx.anim_arctic_glow.play();
                }
            }
        }
    }
    
    // Apply HEAT animations
    if (ctx.label_heat) {
        if (ctx.state == AnimState::FadeOut) {
            ctx.label_heat->setOpa((int)ctx.anim_fadeout.directValue());
        } else {
            ctx.label_heat->setOpa((int)ctx.anim_heat_opa.directValue());
            ctx.label_heat->setY((int)ctx.anim_heat_y.directValue());
        }
    }
    
    // Apply PUMPS animations
    if (ctx.label_pumps) {
        if (ctx.state == AnimState::FadeOut) {
            ctx.label_pumps->setOpa((int)ctx.anim_fadeout.directValue());
        } else {
            ctx.label_pumps->setOpa((int)ctx.anim_pumps_opa.directValue());
            ctx.label_pumps->setY((int)ctx.anim_pumps_y.directValue());
        }
    }
    
    // State machine
    switch (ctx.state) {
        case AnimState::StartupDelay:
            if (elapsed > 300) {
                transition_state(AnimState::ArcticSlide);
            }
            break;
            
        case AnimState::ArcticSlide:
            if (ctx.anim_arctic_x.done() && elapsed > 400) {
                transition_state(AnimState::HeatFade);
            }
            break;
            
        case AnimState::HeatFade:
            if (ctx.anim_heat_opa.done() && elapsed > 300) {
                transition_state(AnimState::PumpsFade);
            }
            break;
            
        case AnimState::PumpsFade:
            if (ctx.anim_pumps_opa.done()) {
                transition_state(AnimState::Hold);
            }
            break;
            
        case AnimState::Hold:
            if (elapsed > 1200) {
                transition_state(AnimState::FadeOut);
            }
            break;
            
        case AnimState::FadeOut:
            if (ctx.anim_fadeout.done()) {
                transition_state(AnimState::Complete);
            }
            break;
            
        case AnimState::Complete:
            cleanup_animation();
            if (ctx.on_complete) {
                ctx.on_complete();
            }
            return false;
            
        default:
            break;
    }
    
    return true;
}

bool startup_anim_is_running(void)
{
    return ctx.running;
}

void startup_anim_stop(void)
{
    cleanup_animation();
}

static void create_particles()
{
    ctx.particles.clear();
    ctx.particles.reserve(NUM_PARTICLES);
    
    lv_obj_t* scr = lv_scr_act();
    
    for (int i = 0; i < NUM_PARTICLES; i++) {
        Particle p;
        p.x = (float)(rand() % ctx.screen_w);
        p.y = (float)(rand() % ctx.screen_h);
        p.speed = 0.5f + (float)(rand() % 20) / 10.0f;  // 0.5 - 2.5
        p.drift_offset = (float)(rand() % 1000);
        p.initial_opa = 80 + (rand() % 120);
        
        int size = 3 + (rand() % 5);  // 3-7 pixels
        
        p.obj = std::make_unique<Container>(scr);
        p.obj->setSize(size, size);
        p.obj->setPos((int)p.x, (int)p.y);
        p.obj->setRadius(LV_RADIUS_CIRCLE);
        p.obj->setBgColor(lv_color_hex(COLOR_PARTICLE));
        p.obj->setBgOpa(p.initial_opa);
        p.obj->setBorderWidth(0);
        p.obj->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
        
        ctx.particles.push_back(std::move(p));
    }
}

static void update_particles()
{
    uint32_t tick = lv_tick_get();
    
    for (auto& p : ctx.particles) {
        // Move down
        p.y += p.speed;
        
        // Gentle horizontal drift
        float drift = sinf((tick + p.drift_offset) / 800.0f) * 1.5f;
        
        // Wrap around
        if (p.y > ctx.screen_h + 10) {
            p.y = -10;
            p.x = (float)(rand() % ctx.screen_w);
        }
        
        // Apply position
        if (p.obj) {
            p.obj->setPos((int)(p.x + drift), (int)p.y);
            
            // Fade out particles during fadeout state
            if (ctx.state == AnimState::FadeOut) {
                int opa = (int)(ctx.anim_fadeout.directValue() * p.initial_opa / 255);
                p.obj->setBgOpa(opa);
            }
        }
    }
}

static void transition_state(AnimState new_state)
{
    ctx.state = new_state;
    ctx.state_start_time = lv_tick_get();
    
    switch (new_state) {
        case AnimState::ArcticSlide: {
            // Start ARCTIC slide from left to pre-calculated center
            ctx.anim_arctic_x.play();
            ctx.anim_arctic_x = (float)ctx.arctic_center_x;
            
            // Start glow pulse
            ctx.anim_arctic_glow.play();
            ctx.anim_arctic_glow = 15;
            ctx.glow_increasing = false;
            break;
        }
            
        case AnimState::HeatFade:
            // Fade in HEAT and move up to target Y
            ctx.anim_heat_opa.play();
            ctx.anim_heat_opa = 255;
            ctx.anim_heat_y.play();
            ctx.anim_heat_y = (float)ctx.heat_pumps_target_y;
            break;
            
        case AnimState::PumpsFade:
            // Fade in PUMPS and move up to target Y
            ctx.anim_pumps_opa.play();
            ctx.anim_pumps_opa = 255;
            ctx.anim_pumps_y.play();
            ctx.anim_pumps_y = (float)ctx.heat_pumps_target_y;
            break;
            
        case AnimState::Hold:
            // Just wait
            break;
            
        case AnimState::FadeOut:
            // Fade everything out
            ctx.anim_fadeout.play();
            ctx.anim_fadeout = 0;
            break;
            
        case AnimState::Complete:
            // Will be handled in update
            break;
            
        default:
            break;
    }
}

static void cleanup_animation()
{
    // Clear all objects (unique_ptr handles deletion)
    ctx.label_arctic.reset();
    ctx.label_heat.reset();
    ctx.label_pumps.reset();
    ctx.particles.clear();
    
    ctx.running = false;
    ctx.state = AnimState::Idle;
}
