/* arch/arm/mach-msm/board-jet-gpio.c
 * Copyright (C) 2011 HTC Corporation.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
*/

#include <mach/gpiomux.h>
#include "board-jet.h"


static struct gpiomux_setting gsbi3 = {
	.func = GPIOMUX_FUNC_1,
	.drv = GPIOMUX_DRV_8MA,
	.pull = GPIOMUX_PULL_NONE,
};

#if 0
static struct gpiomux_setting gsbi4 = {
	.func = GPIOMUX_FUNC_1,
	.drv = GPIOMUX_DRV_8MA,
	.pull = GPIOMUX_PULL_NONE,
};
#endif

static struct gpiomux_setting gsbi8 = {
	.func = GPIOMUX_FUNC_1,
	.drv = GPIOMUX_DRV_8MA,
	.pull = GPIOMUX_PULL_NONE,
};

static struct gpiomux_setting gsbi12 = {
	.func = GPIOMUX_FUNC_1,
	.drv = GPIOMUX_DRV_8MA,
	.pull = GPIOMUX_PULL_NONE,
};

#if 0
static struct gpiomux_setting gsbi10 = {
	.func = GPIOMUX_FUNC_2,
	.drv = GPIOMUX_DRV_4MA,
	.pull = GPIOMUX_PULL_NONE,
};
#endif

static struct gpiomux_setting cdc_mclk = {
	.func = GPIOMUX_FUNC_1,
	.drv = GPIOMUX_DRV_8MA,
	.pull = GPIOMUX_PULL_NONE,
};

static struct gpiomux_setting slimbus = {
	.func = GPIOMUX_FUNC_1,
	.drv = GPIOMUX_DRV_8MA,
	.pull = GPIOMUX_PULL_KEEPER,
};

static struct msm_gpiomux_config jet_gsbi_configs[] __initdata = {
	{
		.gpio      = JET_GPIO_TP_I2C_SDA,	
		.settings = {
			[GPIOMUX_SUSPENDED] = &gsbi3,
		},
	},
	{
		.gpio      = JET_GPIO_TP_I2C_SCL,	
		.settings = {
			[GPIOMUX_SUSPENDED] = &gsbi3,
		},
	},
#if 0 
	{
		.gpio      = JET_GPIO_CAM_I2C_DAT,	
		.settings = {
			[GPIOMUX_SUSPENDED] = &gsbi4,
		},
	},
	{
		.gpio      = JET_GPIO_CAM_I2C_CLK,	
		.settings = {
			[GPIOMUX_SUSPENDED] = &gsbi4,
		},
	},
	{
		
		.gpio      = JET_GPIO_MCAM_SPI_CLK,
		.settings = {
			[GPIOMUX_SUSPENDED] = &gsbi10,
		},
	},
	{
		
		.gpio      = JET_GPIO_MCAM_SPI_CS0,
		.settings = {
			[GPIOMUX_SUSPENDED] = &gsbi10,
		},
	},
	{
		
		.gpio      = JET_GPIO_MCAM_SPI_DI,
		.settings = {
			[GPIOMUX_SUSPENDED] = &gsbi10,
		},
	},
	{
		
		.gpio      = JET_GPIO_MCAM_SPI_DO,
		.settings = {
			[GPIOMUX_SUSPENDED] = &gsbi10,
		},
	},
#endif
	{
		.gpio	   = JET_GPIO_AC_I2C_SDA,	
		.settings = {
			[GPIOMUX_SUSPENDED] = &gsbi8,
		},
	},
	{
		.gpio	   = JET_GPIO_AC_I2C_SCL,	
		.settings = {
			[GPIOMUX_SUSPENDED] = &gsbi8,
		},
	},
	{
		.gpio      = JET_GPIO_SR_I2C_DAT,	
		.settings = {
			[GPIOMUX_SUSPENDED] = &gsbi12,
		},
	},
	{
		.gpio      = JET_GPIO_SR_I2C_CLK,	
		.settings = {
			[GPIOMUX_SUSPENDED] = &gsbi12,
		},
	},
};

static struct msm_gpiomux_config jet_slimbus_configs[] __initdata = {
	{
		.gpio	= JET_GPIO_AUD_WCD_SB_CLK,		
		.settings = {
			[GPIOMUX_SUSPENDED] = &slimbus,
		},
	},
	{
		.gpio	= JET_GPIO_AUD_WCD_SB_DATA,		
		.settings = {
			[GPIOMUX_SUSPENDED] = &slimbus,
		},
	},
};

static struct msm_gpiomux_config jet_audio_codec_configs[] __initdata = {
	{
		.gpio = JET_GPIO_AUD_WCD_MCLK,
		.settings = {
			[GPIOMUX_SUSPENDED] = &cdc_mclk,
		},
	},
};
static struct gpiomux_setting wcnss_5wire_suspend_cfg = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv  = GPIOMUX_DRV_8MA,
	.pull = GPIOMUX_PULL_NONE,
};

static struct gpiomux_setting wcnss_5wire_active_cfg = {
	.func = GPIOMUX_FUNC_1,
	.drv  = GPIOMUX_DRV_8MA,
	.pull = GPIOMUX_PULL_DOWN,
};

static struct msm_gpiomux_config wcnss_5wire_interface[] = {
	{
		.gpio = JET_GPIO_WCN_CMD_DATA2,
		.settings = {
			[GPIOMUX_ACTIVE]    = &wcnss_5wire_active_cfg,
			[GPIOMUX_SUSPENDED] = &wcnss_5wire_suspend_cfg,
		},
	},
	{
		.gpio = JET_GPIO_WCN_CMD_DATA1,
		.settings = {
			[GPIOMUX_ACTIVE]    = &wcnss_5wire_active_cfg,
			[GPIOMUX_SUSPENDED] = &wcnss_5wire_suspend_cfg,
		},
	},
	{
		.gpio = JET_GPIO_WCN_CMD_DATA0,
		.settings = {
			[GPIOMUX_ACTIVE]    = &wcnss_5wire_active_cfg,
			[GPIOMUX_SUSPENDED] = &wcnss_5wire_suspend_cfg,
		},
	},
	{
		.gpio = JET_GPIO_WCN_CMD_SET,
		.settings = {
			[GPIOMUX_ACTIVE]    = &wcnss_5wire_active_cfg,
			[GPIOMUX_SUSPENDED] = &wcnss_5wire_suspend_cfg,
		},
	},
	{
		.gpio = JET_GPIO_WCN_CMD_CLK,
		.settings = {
			[GPIOMUX_ACTIVE]    = &wcnss_5wire_active_cfg,
			[GPIOMUX_SUSPENDED] = &wcnss_5wire_suspend_cfg,
		},
	},
};

static struct gpiomux_setting cam_settings[11] = {
	{
		.func = GPIOMUX_FUNC_GPIO, 
		.drv = GPIOMUX_DRV_8MA,
		.pull = GPIOMUX_PULL_DOWN,
		.dir = GPIOMUX_IN,
	},

	{
		.func = GPIOMUX_FUNC_1, 
		.drv = GPIOMUX_DRV_8MA,
		.pull = GPIOMUX_PULL_NONE,
	},

	{
		.func = GPIOMUX_FUNC_GPIO, 
		.drv = GPIOMUX_DRV_8MA,
		.pull = GPIOMUX_PULL_NONE,
		.dir = GPIOMUX_OUT_LOW,
	},

	{
		.func = GPIOMUX_FUNC_1, 
		.drv = GPIOMUX_DRV_8MA,
		.pull = GPIOMUX_PULL_NONE,
	},

	{
		.func = GPIOMUX_FUNC_2, 
		.drv = GPIOMUX_DRV_8MA,
		.pull = GPIOMUX_PULL_NONE,
	},

	{
		.func = GPIOMUX_FUNC_GPIO, 
		.drv = GPIOMUX_DRV_4MA,
		.pull = GPIOMUX_PULL_DOWN,
		.dir = GPIOMUX_IN,
	},

	{
		.func = GPIOMUX_FUNC_2, 
		.drv = GPIOMUX_DRV_2MA,
		.pull = GPIOMUX_PULL_NONE,
	},

	{
		.func = GPIOMUX_FUNC_GPIO, 
		.drv = GPIOMUX_DRV_2MA,
		.pull = GPIOMUX_PULL_NONE,
		.dir = GPIOMUX_IN,
	},

	{
		.func = GPIOMUX_FUNC_GPIO, 
		.drv = GPIOMUX_DRV_2MA,
		.pull = GPIOMUX_PULL_DOWN,
		.dir = GPIOMUX_IN,
	},

	{
		.func = GPIOMUX_FUNC_GPIO, 
		.drv = GPIOMUX_DRV_2MA,
		.pull = GPIOMUX_PULL_NONE,
		.dir = GPIOMUX_OUT_HIGH,
	},

	{
		.func = GPIOMUX_FUNC_GPIO, 
		.drv = GPIOMUX_DRV_2MA,
		.pull = GPIOMUX_PULL_NONE,
		.dir = GPIOMUX_OUT_LOW,
	},

};

static struct msm_gpiomux_config jet_cam_configs[] = {
	{
		.gpio = JET_GPIO_CAM_MCLK1,
		.settings = {
			[GPIOMUX_ACTIVE]    = &cam_settings[4], 
			[GPIOMUX_SUSPENDED] = &cam_settings[2], 
		},
	},
	{
		.gpio = JET_GPIO_CAM_MCLK0,
		.settings = {
			[GPIOMUX_ACTIVE]    = &cam_settings[1], 
			[GPIOMUX_SUSPENDED] = &cam_settings[2], 
		},
	},
	{
		.gpio = JET_GPIO_CAM_I2C_DAT,
		.settings = {
			[GPIOMUX_ACTIVE]    = &cam_settings[3], 
			[GPIOMUX_SUSPENDED] = &cam_settings[2],
		},
	},
	{
		.gpio = JET_GPIO_CAM_I2C_CLK,
		.settings = {
			[GPIOMUX_ACTIVE]    = &cam_settings[3], 
			[GPIOMUX_SUSPENDED] = &cam_settings[2],
		},
	},
	{
		.gpio = JET_GPIO_RAW_INTR0,
		.settings = {
			[GPIOMUX_ACTIVE]    = &cam_settings[7], 
			[GPIOMUX_SUSPENDED] = &cam_settings[8], 
		},
	},
	{
		.gpio = JET_GPIO_RAW_INTR1,
		.settings = {
			[GPIOMUX_ACTIVE]    = &cam_settings[7], 
			[GPIOMUX_SUSPENDED] = &cam_settings[8], 
		},
	},
	
	{
		.gpio      = JET_GPIO_MCAM_SPI_CLK,
		.settings = {
			[GPIOMUX_ACTIVE] = &cam_settings[6], 
			[GPIOMUX_SUSPENDED] = &cam_settings[10],
		},
	},
	{
		.gpio      = JET_GPIO_MCAM_SPI_CS0,
		.settings = {
			[GPIOMUX_ACTIVE] = &cam_settings[6], 
			[GPIOMUX_SUSPENDED] = &cam_settings[10], 
		},
	},
	{
		.gpio      = JET_GPIO_MCAM_SPI_DI,
		.settings = {
			[GPIOMUX_ACTIVE] = &cam_settings[6], 
			[GPIOMUX_SUSPENDED] = &cam_settings[10],
		},
	},
	{
		.gpio      = JET_GPIO_MCAM_SPI_DO,
		.settings = {
			[GPIOMUX_ACTIVE] = &cam_settings[6], 
			[GPIOMUX_SUSPENDED] = &cam_settings[10],
		},
	},
};

static struct gpiomux_setting mdp_vsync_suspend_cfg = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_2MA,
	.pull = GPIOMUX_PULL_DOWN,
};

static struct gpiomux_setting mdp_vsync_active_cfg = {
	.func = GPIOMUX_FUNC_1,
	.drv = GPIOMUX_DRV_2MA,
	.pull = GPIOMUX_PULL_NONE,
};

static struct msm_gpiomux_config msm8960_mdp_vsync_configs[] __initdata = {
	{
		.gpio = JET_GPIO_LCD_TE,
		.settings = {
			[GPIOMUX_ACTIVE] = &mdp_vsync_active_cfg,
			[GPIOMUX_SUSPENDED] = &mdp_vsync_suspend_cfg,
		},
	}
};

#ifdef CONFIG_FB_MSM_HDMI_MSM_PANEL

static struct gpiomux_setting mhl_i2c_suspend_cfg = {
    .func = GPIOMUX_FUNC_GPIO,
    .drv = GPIOMUX_DRV_8MA,
    .pull = GPIOMUX_PULL_NONE,
};

static struct gpiomux_setting mhl_i2c_active_cfg = {
    .func = GPIOMUX_FUNC_1,
    .drv = GPIOMUX_DRV_8MA,
    .pull = GPIOMUX_PULL_NONE,
};

static struct msm_gpiomux_config jet_mhl_i2c_configs[] __initdata = {
    {
        .gpio = 36,
        .settings = {
            [GPIOMUX_ACTIVE]    = &mhl_i2c_active_cfg,
            [GPIOMUX_SUSPENDED] = &mhl_i2c_suspend_cfg,
        },
    },
    {
        .gpio = 37,
        .settings = {
            [GPIOMUX_ACTIVE]    = &mhl_i2c_active_cfg,
            [GPIOMUX_SUSPENDED] = &mhl_i2c_suspend_cfg,
        },
    },
};

static struct gpiomux_setting mhl_suspend_cfg = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_2MA,
	.pull = GPIOMUX_PULL_NONE,
};

static struct gpiomux_setting mhl_active_1_cfg = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_2MA,
	.pull = GPIOMUX_PULL_UP,
};

static struct gpiomux_setting mhl_active_2_cfg = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_2MA,
	.pull = GPIOMUX_PULL_UP,
};

static struct msm_gpiomux_config jet_mhl_configs[] __initdata = {
	{
		.gpio = JET_GPIO_MHL_RSTz,
		.settings = {
			[GPIOMUX_ACTIVE]    = &mhl_active_1_cfg,
			[GPIOMUX_SUSPENDED] = &mhl_suspend_cfg,
		},
	},
	{
		.gpio = JET_GPIO_MHL_INT,
		.settings = {
			[GPIOMUX_ACTIVE]    = &mhl_active_2_cfg,
			[GPIOMUX_SUSPENDED] = &mhl_suspend_cfg,
		},
	},
};


static struct gpiomux_setting hdmi_suspend_pd_cfg = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_2MA,
	.pull = GPIOMUX_PULL_DOWN,
};
static struct gpiomux_setting hdmi_suspend_np_cfg = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_2MA,
	.pull = GPIOMUX_PULL_NONE,
};

static struct gpiomux_setting hdmi_active_1_cfg = {
	.func = GPIOMUX_FUNC_1,
	.drv = GPIOMUX_DRV_8MA,
	.pull = GPIOMUX_PULL_NONE,
};

static struct gpiomux_setting hdmi_active_2_cfg = {
	.func = GPIOMUX_FUNC_1,
	.drv = GPIOMUX_DRV_2MA,
	.pull = GPIOMUX_PULL_DOWN,
};

static struct msm_gpiomux_config jet_hdmi_configs[] __initdata = {
	{
		.gpio = JET_GPIO_HDMI_DDC_CLK,
		.settings = {
			[GPIOMUX_ACTIVE]    = &hdmi_active_1_cfg,
			[GPIOMUX_SUSPENDED] = &hdmi_suspend_np_cfg,
		},
	},
	{
		.gpio = JET_GPIO_HDMI_DDC_DATA,
		.settings = {
			[GPIOMUX_ACTIVE]    = &hdmi_active_1_cfg,
			[GPIOMUX_SUSPENDED] = &hdmi_suspend_np_cfg,
		},
	},
	{
		.gpio = JET_GPIO_HDMI_HPD,
		.settings = {
			[GPIOMUX_ACTIVE]    = &hdmi_active_2_cfg,
			[GPIOMUX_SUSPENDED] = &hdmi_suspend_pd_cfg,
		},
	},
};
#endif


static struct gpiomux_setting usb_id_cfg = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_2MA,
	.pull = GPIOMUX_PULL_NONE,
};

static struct msm_gpiomux_config cable_detect_usbid_config[] __initdata = {
	{
		.gpio = JET_GPIO_USB_ID1,
		.settings = {
			[GPIOMUX_ACTIVE] = &usb_id_cfg,
			[GPIOMUX_SUSPENDED] = &usb_id_cfg,
		},
	}
};

int __init jet_gpiomux_init(void)
{
	int rc;

	rc = msm_gpiomux_init(NR_GPIO_IRQS);
	if (rc) {
		pr_err(KERN_ERR "msm_gpiomux_init failed %d\n", rc);
		return rc;
	}

	msm_gpiomux_install(jet_cam_configs,
			ARRAY_SIZE(jet_cam_configs));

	msm_gpiomux_install(jet_gsbi_configs,
			ARRAY_SIZE(jet_gsbi_configs));

	msm_gpiomux_install(jet_slimbus_configs,
			ARRAY_SIZE(jet_slimbus_configs));

	msm_gpiomux_install(jet_audio_codec_configs,
			ARRAY_SIZE(jet_audio_codec_configs));

#ifdef CONFIG_FB_MSM_HDMI_MSM_PANEL
	msm_gpiomux_install(jet_mhl_i2c_configs,
			ARRAY_SIZE(jet_mhl_i2c_configs));
	msm_gpiomux_install(jet_hdmi_configs,
			ARRAY_SIZE(jet_hdmi_configs));
	msm_gpiomux_install(jet_mhl_configs,
			ARRAY_SIZE(jet_mhl_configs));
#endif
	msm_gpiomux_install(msm8960_mdp_vsync_configs,
			ARRAY_SIZE(msm8960_mdp_vsync_configs));

	msm_gpiomux_install(wcnss_5wire_interface,
			ARRAY_SIZE(wcnss_5wire_interface));

	msm_gpiomux_install(cable_detect_usbid_config,
			ARRAY_SIZE(cable_detect_usbid_config));

	return 0;
}

