


#include <stdio.h>
#include <stdlib.h>
#include <linux/input.h>
#include <sys/select.h>

#include <linux/fb.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <time.h>


#include <unistd.h>
#include <string.h>
#include <stdint.h>

#include <fcntl.h>
#include "rk_fb.h"
#include "ion.h"
#include "rga.h"

struct fb_context {
    int fd;
    void *fbmem;
    size_t screensize;

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
};


struct ion_context {
    int ion_fd;
    int handle_fd;
    void *vaddr;
    unsigned long phys;
    size_t size;
};

struct fb_context fb0;
struct fb_context fb1;
struct fb_context fb2;


struct ion_context ion_bg;
struct ion_context ion_sprite[2];
struct ion_context ion_shark_fish;
struct ion_context ion_gameover;
struct ion_context ion_number;


int fb_init(struct fb_context *fb, const char *dev)
{
    printf("------------------------------------------------ \n");
    fb->fd = open(dev, O_RDWR);
    if (fb->fd < 0) {
        perror("open fb");
        return -1;
    }
    ioctl(fb->fd, FBIOGET_FSCREENINFO, &fb->finfo);
    ioctl(fb->fd, FBIOGET_VSCREENINFO, &fb->vinfo);
    fb->screensize =
        fb->vinfo.yres_virtual * fb->finfo.line_length;
    return 0;
}

int fb_mmap(struct fb_context *fb)
{
    fb->fbmem = mmap(0,
                     fb->screensize,
                     PROT_READ | PROT_WRITE,
                     MAP_SHARED,
                     fb->fd,
                     0);
    printf("fbmem: %p\n", fb->fbmem);
    fflush(stdout);
    if (fb->fbmem == MAP_FAILED) {
        perror("mmap");
        return -1;
    }
    return 0;
}


int ion_alloc(struct ion_context *ctx, size_t size)
{
    ctx->size = size;

    struct ion_allocation_data alloc = {
        .len = size,
        .align = 0,
        .heap_id_mask = ION_HEAP_TYPE_DMA_MASK,
        .flags = 0,
    };

    if (ioctl(ctx->ion_fd, ION_IOC_ALLOC, &alloc) < 0) {
        perror("ION_IOC_ALLOC");
        return -1;
    }

    struct ion_fd_data fd_data = {0};
    fd_data.handle = alloc.handle;

    if (ioctl(ctx->ion_fd, ION_IOC_SHARE, &fd_data) < 0) {
        perror("ION_IOC_SHARE");
        return -1;
    }

    ctx->handle_fd = fd_data.fd;
    printf("ion buffer allocated, handle fd: %d\n", ctx->handle_fd);
    fflush(stdout);


    return 0;
}
int load_raw_to_ion(struct ion_context *ctx, const char *path, size_t offset)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("open raw");
        return -1;
    }

    // 获取文件大小
    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size == -1) {
        perror("lseek end");
        close(fd);
        return -1;
    }

    // 恢复到文件开头
    if (lseek(fd, 0, SEEK_SET) == -1) {
        perror("lseek set");
        close(fd);
        return -1;
    }

    // 检查文件大小是否超过可用空间
    if (offset + file_size > ctx->size) {
        fprintf(stderr, "File too large: need %zu bytes but only %zu bytes available\n", 
                (size_t)(offset + file_size), ctx->size);
        close(fd);
        return -1;
    }

    ssize_t total = 0;
    ssize_t ret;
    uint8_t *dest_addr = (uint8_t*)ctx->vaddr + offset;

    while (total < file_size) {
        ret = read(fd, dest_addr + total, file_size - total);
        if (ret <= 0) {
            if (ret == 0) {
                fprintf(stderr, "Unexpected EOF: read %zd of %jd bytes\n", 
                        total, (intmax_t)file_size);
            } else {
                perror("read raw");
            }
            close(fd);
            return -1;
        }
        total += ret;
    }

    printf("raw image loaded: %zd bytes at offset %zu\n", total, offset);
    close(fd);
    return 0;
}

int map_ion_buffer(struct ion_context *ctx)
{
    ctx->vaddr = mmap(NULL,
                    ctx->size,
                    PROT_READ | PROT_WRITE,
                    MAP_SHARED,
                    ctx->handle_fd,
                    0);

    if (ctx->vaddr == MAP_FAILED) {
        perror("ion mmap");
        return -1;
    }
    printf("ion buffer mapped %p\n", ctx->vaddr);
    return 0;
}
int set_color(struct ion_context *ctx, uint32_t c)
{    
    int i;
    for (i = 0; i < ctx->size / 4 ; i++) {
        ((uint32_t*)ctx->vaddr)[i] = c;
    }
    return 0;
}

int rga_blit(int fd_rga, int ion_fd_src, int ion_fd_dst, int s_x, int s_y, int d_x, int d_y, int act_w, int act_h, int vir_w_src, int vir_h_src, int vir_w_dst, int vir_h_dst, int alpha)
{
    struct rga_req req;
    memset(&req,0,sizeof(req));

    req.src.yrgb_addr = ion_fd_src;
    req.src.act_w = act_w;
    req.src.act_h = act_h;
    req.src.vir_w = vir_w_src;
    req.src.vir_h = vir_h_src;
    req.src.format = RK_FORMAT_RGBA_8888;
    req.src.x_offset = s_x;
    req.src.y_offset = s_y;


    req.dst.yrgb_addr = ion_fd_dst;
    req.dst.act_w = act_w;
    req.dst.act_h = act_h;
    req.dst.format = RK_FORMAT_RGBA_8888;
    req.dst.x_offset = d_x;
    req.dst.y_offset = d_y;
    req.dst.vir_w = vir_w_dst;
    req.dst.vir_h = vir_h_dst;

    req.clip.xmin = 0;
    req.clip.xmax = vir_w_dst - 1;
    req.clip.ymin = 0;
    req.clip.ymax = vir_h_dst - 1;

    req.scale_mode = 0;
    req.render_mode = bitblt_mode;
    if (alpha) {
        req.alpha_rop_flag = 0
        | (1 << 0)  // bit0 = 1: alpha_rop_enable - 启用alpha混合
        | (0 << 1)  // bit1 = 0: rop enable - 禁用ROP光栅操作
        | (0 << 2)  // bit2 = 0: fading_enable - 禁用fading
        | (0 << 3)  // bit3 = 0: PD_enable - 禁用Porter-Duff（除非需要特殊混合模式）
        | (1 << 4)  // bit4 = 0: alpha cal_mode_sel - 选择alpha计算模式
        | (0 << 5)  // bit5 = 0: dither_enable - 禁用抖动
        | (0 << 6)  // bit6 = 0: gradient fill - 禁用渐变填充
        | (0 << 7); // bit7 = 0: AA_enable - 根据需要决定是否抗锯齿
        req.alpha_rop_mode = 0
        | (0x1 << 0)  // bit[0~1]: alpha模式 - 00表示使用源图像的alpha
        | (0x0 << 1)  // bit[0~1]: alpha模式 - 00表示使用源图像的alpha
        | (0x0 << 2)  // bit[2~3]: rop模式 - 不使用
        | (0x0 << 4)  // bit4: zero mode en - 禁用zero模式
        | (0x0 << 5); // bit5: dst alpha mode - 不使用目标alpha;

        req.alpha_global_value = 0xFF;  // 0x00~0xFF，0完全透明，0xFF完全不透明
    } else {
        req.alpha_rop_flag = 0;
    }
 
   
    ioctl(fd_rga, RGA_BLIT_SYNC, &req);

    return 0;
}
int rga_clear(int fd_rga,
              int ion_fd_dst,
              int width,
              int height,
              int vir_w,
              int vir_h,
              uint32_t color)
{
    struct rga_req req;
    memset(&req, 0, sizeof(req));

    // 目标 buffer
    req.dst.yrgb_addr = ion_fd_dst;
    req.dst.format = RK_FORMAT_RGBA_8888;

    req.dst.act_w = width;
    req.dst.act_h = height;

    req.dst.vir_w = vir_w;
    req.dst.vir_h = vir_h;

    req.dst.x_offset = 0;
    req.dst.y_offset = 0;

    // 裁剪区域（全屏）
    req.clip.xmin = 0;
    req.clip.ymin = 0;
    req.clip.xmax = vir_w - 1;
    req.clip.ymax = vir_h - 1;

    req.render_mode = color_fill_mode;
    // 填充颜色
    req.fg_color = color;
    // 不用 src
    req.src.yrgb_addr = 0;

    if (ioctl(fd_rga, RGA_BLIT_SYNC, &req) < 0) {
        perror("RGA clear failed");
        return -1;
    }

    return 0;
}


int init_fb(struct rk_fb_win_cfg_data *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    
    // //bg
    cfg->wait_fs = 1;
    // cfg->win_par[0].win_id = 0;
    // cfg->win_par[0].z_order = 0;
    // cfg->win_par[0].area_par[0].data_format = HAL_PIXEL_FORMAT_RGBA_8888;        
    // cfg->win_par[0].area_par[0].ion_fd = ion_bg.handle_fd;
    // cfg->win_par[0].area_par[0].acq_fence_fd = -1;
    // cfg->win_par[0].area_par[0].x_offset = 0;
    // cfg->win_par[0].area_par[0].y_offset = 0;
    // cfg->win_par[0].area_par[0].xpos = 0;
    // cfg->win_par[0].area_par[0].ypos = 0;
    // cfg->win_par[0].area_par[0].xsize = 1280;
    // cfg->win_par[0].area_par[0].ysize = 720;
    // cfg->win_par[0].area_par[0].xact = 1280;
    // cfg->win_par[0].area_par[0].yact = 720;
    // cfg->win_par[0].area_par[0].xvir = 2560;
    // cfg->win_par[0].area_par[0].yvir = 720;


    // //sprite
    // cfg->win_par[1].win_id = 1;
    // cfg->win_par[1].z_order = 1;
    // cfg->win_par[1].area_par[0].data_format = HAL_PIXEL_FORMAT_RGBA_8888;        
    // cfg->win_par[1].area_par[0].ion_fd = ion_sprite[0].handle_fd;
    // cfg->win_par[1].area_par[0].acq_fence_fd = -1;
    // cfg->win_par[1].area_par[0].x_offset = 0; //缓冲区中的偏移，单位像素
    // cfg->win_par[1].area_par[0].y_offset = 0;
    // cfg->win_par[1].area_par[0].xpos = 0; //屏幕上的位置，单位像素
    // cfg->win_par[1].area_par[0].ypos = 0;
    // cfg->win_par[1].area_par[0].xsize = 1280; //显示的宽高，单位像素
    // cfg->win_par[1].area_par[0].ysize = 720;
    // cfg->win_par[1].area_par[0].xact = 1280; //缓冲区中的有效宽高，单位像素，不包含padding
    // cfg->win_par[1].area_par[0].yact = 720;
    // cfg->win_par[1].area_par[0].xvir = 1280;  //缓冲区中实际的宽高，单位像素，包含可能因为对齐产生的padding
    // cfg->win_par[1].area_par[0].yvir = 720;

    
    //sprite
    cfg->win_par[0].win_id = 1;
    cfg->win_par[0].z_order = 1;
    cfg->win_par[0].area_par[0].data_format = HAL_PIXEL_FORMAT_RGBA_8888;        
    cfg->win_par[0].area_par[0].ion_fd = ion_sprite[0].handle_fd;
    cfg->win_par[0].area_par[0].acq_fence_fd = -1;
    cfg->win_par[0].area_par[0].x_offset = 0; //缓冲区中的偏移，单位像素
    cfg->win_par[0].area_par[0].y_offset = 0;
    cfg->win_par[0].area_par[0].xpos = 0; //屏幕上的位置，单位像素
    cfg->win_par[0].area_par[0].ypos = 0;
    cfg->win_par[0].area_par[0].xsize = 1280; //显示的宽高，单位像素
    cfg->win_par[0].area_par[0].ysize = 720;
    cfg->win_par[0].area_par[0].xact = 1280; //缓冲区中的有效宽高，单位像素，不包含padding
    cfg->win_par[0].area_par[0].yact = 720;
    cfg->win_par[0].area_par[0].xvir = 1280;  //缓冲区中实际的宽高，单位像素，包含可能因为对齐产生的padding
    cfg->win_par[0].area_par[0].yvir = 720;

    
    

    return 0;
}
int show_buffer(struct fb_context *fb, struct ion_context *ion, struct rk_fb_win_cfg_data *cfg)
{ 
    // cfg->win_par[1].area_par[0].ion_fd = ion->handle_fd;
    // cfg->win_par[1].area_par[0].acq_fence_fd = -1;
    cfg->win_par[0].area_par[0].ion_fd = ion->handle_fd;
    cfg->win_par[0].area_par[0].acq_fence_fd = -1;
    ioctl(fb->fd, RK_FBIOSET_CONFIG_DONE, cfg);
    return 0;
}

int draw_bg(struct ion_context *ion, int fd_rga, int speed)
{
    static int i_bg = 0;
    if (i_bg >= 1280) i_bg -= 1280;
    int x_bg = i_bg * speed;
    rga_blit(fd_rga, ion_bg.handle_fd, ion->handle_fd, x_bg, 0, 0, 0, 1280, 720, 2560, 720, 1280, 720, 0);
    i_bg ++;
    return 0;
}

#include <stdio.h>

#define MAX_DIGITS 10

// 返回数字位数，digits数组里按“从高位到低位”存
int split_digits(int num, int *digits)
{
    if (num == 0) {
        digits[0] = 0;
        return 1;
    }

    int temp[MAX_DIGITS];
    int count = 0;

    // 先拆成低位在前
    while (num > 0 && count < MAX_DIGITS) {
        temp[count++] = num % 10;
        num /= 10;
    }

    // 再反转成高位在前
    int i = 0;
    for (i = 0; i < count; i++) {
        digits[i] = temp[count - 1 - i];
    }

    return count;
}
int nums[10];
// int rga_blit(int fd_rga, int ion_fd_src, int ion_fd_dst, int s_x, int s_y, int d_x, int d_y, int act_w, int act_h, int vir_w_src, int vir_h_src, int vir_w_dst, int vir_h_dst, int alpha)
int draw_score(struct ion_context *ion, int fd_rga, int score)
{
    int i = 0;
    int num_cnt = split_digits(score, nums);
    int x_number = 0;
    int posx = 0;
    int posy = 30;
    int x_offset = 10;
    for (i = 0; i < num_cnt; i ++) {
        int idx = nums[i];
        x_number = idx * 40;
        posx = x_offset + i*40;
        rga_blit(fd_rga, ion_number.handle_fd, ion->handle_fd, x_number, 0, posx, posy, 40, 49, 400, 49, 1280, 720, 1);
    }
    
    return 0;
}
int w_shark = 1120 / 4;
int h_shark = 286 / 2;
int draw_shark(struct ion_context *ion, int fd_rga, int posx, int posy, int w_shark, int h_shark)
{
    static int i_shark = 0;
    int x_shark = 0;
    int y_shark = 0;
    if (i_shark == 8) i_shark = 0;
    x_shark = 0 + w_shark * (i_shark % 4);
    y_shark = 0 + h_shark * ((i_shark / 4) % 2);
    rga_blit(fd_rga, ion_shark_fish.handle_fd, ion->handle_fd, x_shark, y_shark, posx, posy, w_shark, h_shark, 1120, 350, 1280, 720, 1);

    i_shark ++;
    return 0;
}

#define PIC_H 350
#define OFFSET_Y_FISH 286

int h_fish = PIC_H - OFFSET_Y_FISH;
int w_fish = 1120 / 10;
int draw_fish(struct ion_context *ion, int fd_rga, int posx, int posy, int w_fish, int h_fish)
{
    static int i_fish = 0;

    if (i_fish == 4) i_fish = 0;
    int x_fish = 0 + w_fish * i_fish;
    int y_fish = OFFSET_Y_FISH;
    rga_blit(fd_rga, ion_shark_fish.handle_fd, ion->handle_fd, x_fish, y_fish, posx, posy, w_fish, h_fish, 1120, 350, 1280, 720, 1);

    i_fish ++;
    return 0;
}
int draw_game_over(struct ion_context *ion, int fd_rga, int posx, int posy, int w_gameover, int h_gameover)
{
    rga_blit(fd_rga, ion_gameover.handle_fd, ion->handle_fd, 0, 0, posx, posy, w_gameover, h_gameover, w_gameover, h_gameover, 1280, 720, 1);

    return 0;
}
static inline long long time_us()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000000LL + tv.tv_usec;
}
static int key_up_count = 0;
static int key_up_pressed = 0;
void ir_poll(int fd)
{
    struct input_event ev;
    key_up_count = 0;
    while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
        // printf("Event: type=%d, code=%d, value=%d\n", ev.type, ev.code, ev.value);
        if (ev.type == EV_KEY) {
            if (ev.code == KEY_UP) {
                if (ev.value == 1) {
                    key_up_count++;
                    key_up_pressed = 1;
                } else if (ev.value == 0) {
                    key_up_pressed = 0;
                }
            }
        }
    }
}

static int check_collision(int x1, int y1, int w1, int h1,
                    int x2, int y2, int w2, int h2)
{
    if (x1 + w1 <= x2) return 0;
    if (x2 + w2 <= x1) return 0;
    if (y1 + h1 <= y2) return 0;
    if (y2 + h2 <= y1) return 0;
    return 1;
}
static int check_collision_with_shrink(int x1, int y1, int w1, int h1,
                    int x2, int y2, int w2, int h2, int shrink)
{
    return check_collision(
                x1 + shrink,
                y1 + shrink,
                w1 - 2 * shrink,
                h1 - 2 * shrink,
                x2 + shrink,
                y2 + shrink,
                w2 - 2 * shrink,
                h2 - 2 * shrink
            );
}
static void fix_posy_fish(int py_f2, int py_f1, int h_fish, int* posy){
    if (py_f2 + h_fish >= py_f1 &&
        py_f2 <= py_f1 + h_fish) {
        if(*posy > 3 * h_fish) { 
            *posy -= 2 * h_fish;
        } else {
            *posy += 2 * h_fish;
        }
    }
}
int main()
{
    int easy = 0;
    struct input_event ev;
    int score = 0;
    int fd_rc = open("/dev/input/event0", O_RDONLY);
    fcntl(fd_rc, F_SETFL, O_NONBLOCK);
    int fd_rga = open("/dev/rga", O_RDWR);
    int ion_fd = open("/dev/ion", O_RDWR);
    if (ion_fd < 0) {
        perror("open ion");
        return -1;
    }
    ion_bg.ion_fd = ion_fd;
    ion_sprite[0].ion_fd = ion_fd;
    ion_sprite[1].ion_fd = ion_fd;
    ion_shark_fish.ion_fd = ion_fd;
    ion_gameover.ion_fd = ion_fd;
    ion_number.ion_fd = ion_fd;
    ////
    fb_init(&fb0, "/dev/fb0");
    fb_init(&fb1, "/dev/fb1");

    size_t size_bg = 1280 * 2 * 720 * 4; // 2560x720, RGBA8888
    size_t size_sprite = 1280 * 720 * 4; // 1280x720, RGBA8888
    size_t size_shark_fish = 1120 * 350 * 4; // 1120x350, RGBA8888
    size_t size_gameover = 221 * 131 * 4; // 221x131, RGBA8888
    size_t size_number = 400 * 49 * 4; // 400x49, RGBA8888


    ion_alloc(&ion_bg, size_bg);
    ion_alloc(&ion_sprite[0], size_sprite);
    ion_alloc(&ion_sprite[1], size_sprite);
    ion_alloc(&ion_shark_fish, size_shark_fish);
    ion_alloc(&ion_gameover, size_gameover);
    ion_alloc(&ion_number, size_number);

    map_ion_buffer(&ion_bg);
    map_ion_buffer(&ion_sprite[0]);
    map_ion_buffer(&ion_sprite[1]);
    map_ion_buffer(&ion_shark_fish);
    map_ion_buffer(&ion_gameover);
    map_ion_buffer(&ion_number);



    load_raw_to_ion(&ion_bg, "/usr/bin/bg_2560_720.raw", 0);
    load_raw_to_ion(&ion_shark_fish, "/usr/bin/shark_fish_1120_350.raw", 0);
    load_raw_to_ion(&ion_gameover, "/usr/bin/gameover_221_131.raw", 0);
    load_raw_to_ion(&ion_number, "/usr/bin/number_400_49.raw", 0);

    // set_color(&ion_gameover, 0x00ff00ff); // 不显示
    // set_color(&ion_gameover, 0xffff0000);// 半透蓝
    // set_color(&ion_gameover, 0xff00ff00); //半透绿
    // set_color(&ion_gameover, 0xff0000ff); //半透红
    

    struct rk_fb_win_cfg_data cfg;
    init_fb(&cfg);

    ioctl(fb1.fd, FBIOGET_VSCREENINFO, &fb1.vinfo);
    // printf("fb1 vinfo after put: xres=%d, yres=%d, nonstd=0x%x, xres_virtual=%d, yres_virtual=%d, xoffset=%d, yoffset=%d, grayscal=%d\n",
    //         fb1.vinfo.xres, fb1.vinfo.yres, fb1.vinfo.nonstd, fb1.vinfo.xres_virtual, fb1.vinfo.yres_virtual, fb1.vinfo.xoffset, fb1.vinfo.yoffset, fb1.vinfo.grayscale);
   

    int shar_start = 150;
    int posx_shark = 200;
    int posy_shark = shar_start;
    int idx_dualbuffer = 0;
    int i_frame = 0;
    int last_rel_fence = -1;
    long long last_frame_time = time_us();
    int posx_bg = 0;
    int w_fish = 1120 / 10;
    int fish_start = 1280 - w_fish;
    int posx_fish1 = (fish_start + (posx_shark + w_shark)) / 2;
    int posx_fish2 = fish_start;
    int posy_fish1 = rand() % (600 - h_fish);
    int posy_fish2 = rand() % (600 - h_fish);

    float speed_bg = 20;
    float speed_shark_ctr = 5.0;
    
    struct ion_context *ion_sprite_cur = (idx_dualbuffer == 0) ? &ion_sprite[0] : &ion_sprite[1];
    struct ion_context *ion_sprite_next = (idx_dualbuffer == 0) ? &ion_sprite[1] : &ion_sprite[0];
    init:
        score = 0;
        posy_shark = shar_start;
        posx_fish1 = (fish_start + (posx_shark + w_shark)) / 2;
        posx_fish2 = fish_start;


        posy_fish1 = rand() % (600 - h_fish);
        posy_fish2 = rand() % (600 - h_fish);
        fix_posy_fish(posy_fish2, posy_fish1, h_fish, &posy_fish2);

    while (1) {
        long long current_time = time_us();
        long long time_since_last_frame = current_time - last_frame_time;
        if(time_since_last_frame < 150000) { // 150ms
            usleep(5000); // 5ms
            continue;
        }
        ir_poll(fd_rc);
        // posx_bg += speed_bg;
        posx_fish1 -= speed_bg;
        posx_fish2 -= speed_bg;
        if(posx_bg > 1280) posx_bg -= 1280;
        // fish missed 
        if(posx_fish1 < posx_shark - w_fish || posx_fish2 < posx_shark - w_fish) {
            posx_fish1 = fish_start;
            posy_fish1 = rand() % (600 - h_fish);
            posx_fish2 = fish_start;
            posy_fish2 = rand() % (600 - h_fish);
            if(easy != 1) {
                goto game_over;
            }
        } 
        
        if (i_frame == 8) i_frame = 0;
        ion_sprite_cur = (idx_dualbuffer == 0) ? &ion_sprite[0] : &ion_sprite[1];
        ion_sprite_next = (idx_dualbuffer == 0) ? &ion_sprite[1] : &ion_sprite[0];
        // long long start_time = time_us();
        
        // cfg.win_par[0].area_par[0].x_offset = posx_bg;
        show_buffer(&fb1, ion_sprite_cur, &cfg);
        // long long end_time = time_us();
        // printf("Frame %d displayed, time taken: %lld us\n", i_frame, end_time - start_time);

        if (key_up_count > 0) {
            posy_shark -= 10 * speed_shark_ctr * key_up_count;
        } else if (key_up_pressed) {
            posy_shark -= 10 * speed_shark_ctr;
        } else {
            posy_shark += 4 * speed_shark_ctr;
        }
        if (posy_shark < 0) posy_shark = 0;
        if (posy_shark > 720 - 286 / 2) {
            posy_shark = 720 - 286 / 2;
            if (easy != 1)
                goto game_over;
        }
        // start_time = time_us();
        // rga_clear(fd_rga, ion_sprite_next->handle_fd, 1280, 720, 1280, 720, 0xffffffff);
        // rga_clear(fd_rga, ion_sprite_next->handle_fd, 1280, 720, 1280, 720, 0x00000000);
        draw_bg(ion_sprite_next, fd_rga, speed_bg);
        draw_score(ion_sprite_next, fd_rga, score);
        // end_time = time_us();
        // printf("RGA clear time: %lld us\n", end_time - start_time);
        // start_time = time_us();
        draw_shark(ion_sprite_next, fd_rga, posx_shark, posy_shark, w_shark, h_shark);
     
        // end_time = time_us();
        // printf("RGA draw_shark time: %lld us\n", end_time - start_time);
        // start_time = time_us();
        int shrink_y_shark = 40;
        if ( check_collision(posx_shark + 10, posy_shark + shrink_y_shark, w_shark - 2* 10, h_shark - 2 * shrink_y_shark,
                            posx_fish1 + 10, posy_fish1 + 10, w_fish - 2 * 10, h_fish - 2* 10)) {
            posx_fish1 = fish_start;
            posy_fish1 = rand() % (600 - h_fish);
            fix_posy_fish(posy_fish2, posy_fish1, h_fish, &posy_fish1);
            score++;
            // printf("score: %d\n", score);
        }
        draw_fish(ion_sprite_next, fd_rga, posx_fish1, posy_fish1, w_fish, h_fish);
        // end_time = time_us();
        // printf("RGA draw_fish time: %lld us\n", end_time - start_time);
        // start_time = time_us();
        
        if ( check_collision(posx_shark + 10, posy_shark + shrink_y_shark, w_shark - 2* 10, h_shark - 2 * shrink_y_shark,
                            posx_fish2 + 10, posy_fish2 + 10, w_fish - 2 * 10, h_fish - 2* 10)) {

            posx_fish2 = fish_start;
            posy_fish2 = rand() % (600 - h_fish);
            fix_posy_fish(posy_fish2, posy_fish1, h_fish, &posy_fish2);
            score++;
            
            // printf("score: %d\n", score);
        }
           
        draw_fish(ion_sprite_next, fd_rga, posx_fish2, posy_fish2, w_fish, h_fish);
        // end_time = time_us();
        // printf("RGA draw_fish time: %lld us\n", end_time - start_time);
        int i;
        for ( i = 0; i < RK_MAX_BUF_NUM; i ++) {
            if (cfg.rel_fence_fd[i] >= 0) {
                // printf("cfg.rel_fence_fd[%d] = %d\n", i, cfg.rel_fence_fd[i]);
                close(cfg.rel_fence_fd[i]);
            }
        }
        if (cfg.ret_fence_fd >= 0) {
            // printf("cfg.ret_fence_fd = %d\n", cfg.ret_fence_fd);
            close(cfg.ret_fence_fd);
        }
        
        idx_dualbuffer ^= 1;
        i_frame++;
        
        last_frame_time = current_time;
    }

game_over:
    draw_game_over(ion_sprite_cur, fd_rga, 1280 / 2 - 221 / 2, 720 / 2 - 131 / 2, 221, 131);
    show_buffer(&fb1, ion_sprite_cur, &cfg);
    printf("Game Over! Final Score: %d\n", score);
    while (1) {
        if(read(fd_rc, &ev, sizeof(ev)) == sizeof(ev))
        {
            // printf("Event: type=%d, code=%d, value=%d\n", ev.type, ev.code, ev.value);
            if (ev.type == EV_KEY) {
                if (ev.code == KEY_DOWN) {
                    if (ev.value == 1) {
                    } else if (ev.value == 0) {
                        goto init;
                    }
                }
            
            }
        }
        usleep(100000);
    }
    return 0;
}
