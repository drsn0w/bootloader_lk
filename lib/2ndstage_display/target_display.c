#include <err.h>
#include <reg.h>
#include <dev/fbcon.h>
#include <debug.h>
#include <stdlib.h>
#include <string.h>
#include <mipi_dsi.h>
#include <platform.h>
#include <platform/timer.h>

#if defined(WITH_LIB_2NDSTAGE_DISPLAY_MDP3)
#include <mdp3.h>
#elif defined(WITH_LIB_2NDSTAGE_DISPLAY_MDP4)
#include <mdp4.h>
#elif defined(WITH_LIB_2NDSTAGE_DISPLAY_MDP5)
#include <mdp5.h>
#else
#error "MDP is not supported by this platform"
#endif

#include <platform/iomap.h>
#include <dev/lcdc.h>

/* Function to check if the memory address range falls within the aboot
 * boundaries.
 * start: Start of the memory region
 * size: Size of the memory region
 */
static int check_aboot_addr_range_overlap(uint32_t start, uint32_t size)
{
    /* Check for boundary conditions. */
    if ((UINT_MAX - start) < size)
        return -1;

    /* Check for memory overlap. */
    if ((start < MEMBASE) && ((start + size) <= MEMBASE))
        return 0;
    else if (start >= (MEMBASE + MEMSIZE))
        return 0;
    else
        return -1;
}

#if defined(WITH_LIB_2NDSTAGE_DISPLAY_MDP3)
static void mdss_mdp_cmd_kickoff(void) {
	writel(0x00000001, MDP_DMA_P_START);
	mdelay(10);
}

int mdp_dump_config(struct fbcon_config *fb)
{
    fb->base = (void *) readl(MDP_DMA_P_BUF_ADDR);
    fb->width = readl(MDP_DMA_P_BUF_Y_STRIDE)/3;
    fb->height = readl(MDP_DMA_P_SIZE)>>16;
    fb->stride = fb->width;
    fb->bpp = 24;
    fb->format = FB_FORMAT_RGB888;

#ifdef WITH_2NDSTAGE_DISPLAY_DMA_TRIGGER
    fb->update_start = mdss_mdp_cmd_kickoff;
#endif

    return NO_ERROR;
}

#elif defined(WITH_LIB_2NDSTAGE_DISPLAY_MDP4)
extern void mdp_start_dma(void);

static void mipi_update_sw_trigger(void) {
	mdp_start_dma();
	dsb();
	mdelay(10);
	dsb();
	writel(0x1, DSI_CMD_MODE_MDP_SW_TRIGGER);
}

int mdp_dump_config(struct fbcon_config *fb)
{
    fb->base = (void *) readl(MDP_DMA_P_BUF_ADDR);
    fb->width = readl(MDP_DMA_P_BUF_Y_STRIDE)/3;
    fb->height = readl(MDP_DMA_P_SIZE)>>16;
    fb->stride = fb->width;
    fb->bpp = 24;
    fb->format = FB_FORMAT_RGB888;

    uint32_t trigger_ctrl = readl(DSI_TRIG_CTRL);
    int te_sel = (trigger_ctrl >> 31) & 1;
    int mdp_trigger = (trigger_ctrl >> 4) & 4;
    int dma_trigger = (trigger_ctrl) & 4;

    dprintf(INFO, "%s: trigger_ctrl=0x%x te_sel=%d mdp_trigger=%d dma_trigger=%d\n",
            __func__, trigger_ctrl, te_sel, mdp_trigger, dma_trigger);

#ifdef DISPLAY_RGBSWAP
    unsigned char DST_FORMAT = 8;
    int data = 0x00100000;
    data |= ((DISPLAY_RGBSWAP & 0x07) << 16);
    writel(data | DST_FORMAT, DSI_COMMAND_MODE_MDP_CTRL);
#endif

    if (mdp_trigger == DSI_CMD_TRIGGER_SW) {
        fb->update_start = mipi_update_sw_trigger;
    }

    return NO_ERROR;
}

#elif defined(WITH_LIB_2NDSTAGE_DISPLAY_MDP5)
static int mdp_dump_pipe_info(uint32_t *pipe_type, uint32_t *dual_pipe)
{
    int rc;

    *dual_pipe = 0;

    if (readl(MDP_VP_0_VIG_0_BASE + PIPE_SSPP_SRC0_ADDR)) {
        *pipe_type = MDSS_MDP_PIPE_TYPE_VIG;
        rc = 0;
    }
    else if (readl(MDP_VP_0_RGB_0_BASE + PIPE_SSPP_SRC0_ADDR)) {
        *pipe_type = MDSS_MDP_PIPE_TYPE_RGB;
        rc = 0;
    }
    else if (readl(MDP_VP_0_DMA_0_BASE + PIPE_SSPP_SRC0_ADDR)) {
        *pipe_type = MDSS_MDP_PIPE_TYPE_DMA;
        rc = 0;
    }
    else {
        rc = -1;
    }

    return rc;
}

static void mdp_select_pipe_type(uint32_t pipe_type, uint32_t *left_pipe, uint32_t *right_pipe)
{
    switch (pipe_type) {
        case MDSS_MDP_PIPE_TYPE_RGB:
            *left_pipe = MDP_VP_0_RGB_0_BASE;
            *right_pipe = MDP_VP_0_RGB_1_BASE;
            break;
        case MDSS_MDP_PIPE_TYPE_DMA:
            *left_pipe = MDP_VP_0_DMA_0_BASE;
            *right_pipe = MDP_VP_0_DMA_1_BASE;
            break;
        case MDSS_MDP_PIPE_TYPE_VIG:
        default:
            *left_pipe = MDP_VP_0_VIG_0_BASE;
            *right_pipe = MDP_VP_0_VIG_1_BASE;
            break;
    }
}

int mdp_dma_on(struct msm_panel_info *pinfo);

#ifdef WITH_2NDSTAGE_DISPLAY_DMA_TRIGGER
static struct msm_panel_info pinfo = {0};
static void mdss_mdp_cmd_kickoff(void)
{
    mdp_dma_on(&pinfo);
    dsb();
    mdelay(32);
}
#endif

int mdp_dump_config(struct fbcon_config *fb)
{
    int rc;
    uint32_t pipe_type;
    uint32_t dual_pipe;
    uint32_t left_pipe;
    uint32_t right_pipe;

    // dump pipe info
    rc = mdp_dump_pipe_info(&pipe_type, &dual_pipe);
    if(rc) {
        return -1;
    }
    dprintf(INFO, "%s: pipe_type=%u dual_pipe=%u\n", __func__, pipe_type, dual_pipe);

    // select pipes
    mdp_select_pipe_type(pipe_type, &left_pipe, &right_pipe);
    uint32_t pipe_base = left_pipe;

    fb->base = (void *) readl(pipe_base + PIPE_SSPP_SRC0_ADDR);
    fb->width = readl(pipe_base + PIPE_SSPP_SRC_YSTRIDE)/3;
    fb->height = readl(pipe_base + PIPE_SSPP_SRC_IMG_SIZE)>>16;
    fb->stride = fb->width;
    fb->bpp = 24;
    fb->format = FB_FORMAT_RGB888;

#ifdef WITH_2NDSTAGE_DISPLAY_DMA_TRIGGER
    pinfo.pipe_type = pipe_type;
    pinfo.dest = DISPLAY_1;

    fb->update_start = mdss_mdp_cmd_kickoff;
#endif

    return NO_ERROR;
}
#endif

static struct fbcon_config config;

void target_display_init(const char *panel_name)
{
    int rc;

    // clear config
    memset(&config, 0, sizeof(config));

    // dump config
    rc = mdp_dump_config(&config);

    // dump failed
    if (rc || config.base==NULL || config.width==0 || config.height==0 ||
            check_aboot_addr_range_overlap((uint32_t)config.base, config.width*config.height*config.bpp/3)) {
        dprintf(CRITICAL, "Invalid config dumped: base=%p size=%ux%ux%u\n", config.base, config.width, config.height, config.bpp);
        return;
    }

    // setup fbcon
    fbcon_setup(&config);

    // display logo
    display_image_on_screen();
}
