/**
 *
 * \file
 *
 * \brief This module contains NMC1000 bus wrapper APIs implementation.
 *
 * Copyright (c) 2016-2021 Microchip Technology Inc. and its subsidiaries.
 *
 * \asf_license_start
 *
 * \page License
 *
 * Subject to your compliance with these terms, you may use Microchip
 * software and any derivatives exclusively with Microchip products.
 * It is your responsibility to comply with third party license terms applicable
 * to your use of third party software (including open source software) that
 * may accompany Microchip software.
 *
 * THIS SOFTWARE IS SUPPLIED BY MICROCHIP "AS IS". NO WARRANTIES,
 * WHETHER EXPRESS, IMPLIED OR STATUTORY, APPLY TO THIS SOFTWARE,
 * INCLUDING ANY IMPLIED WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY,
 * AND FITNESS FOR A PARTICULAR PURPOSE. IN NO EVENT WILL MICROCHIP BE
 * LIABLE FOR ANY INDIRECT, SPECIAL, PUNITIVE, INCIDENTAL OR CONSEQUENTIAL
 * LOSS, DAMAGE, COST OR EXPENSE OF ANY KIND WHATSOEVER RELATED TO THE
 * SOFTWARE, HOWEVER CAUSED, EVEN IF MICROCHIP HAS BEEN ADVISED OF THE
 * POSSIBILITY OR THE DAMAGES ARE FORESEEABLE.  TO THE FULLEST EXTENT
 * ALLOWED BY LAW, MICROCHIP'S TOTAL LIABILITY ON ALL CLAIMS IN ANY WAY
 * RELATED TO THIS SOFTWARE WILL NOT EXCEED THE AMOUNT OF FEES, IF ANY,
 * THAT YOU HAVE PAID DIRECTLY TO MICROCHIP FOR THIS SOFTWARE.
 *
 * \asf_license_stop
 *
 */

#include <stdio.h>
#include "bsp/include/nm_bsp.h"
#include "common/include/nm_common.h"
#include "bus_wrapper/include/nm_bus_wrapper.h"
#include "asf.h"
#include "conf_winc.h"

#define NM_BUS_MAX_TRX_SZ	256

tstrNmBusCapabilities egstrNmBusCapabilities =
{
	NM_BUS_MAX_TRX_SZ
};

#ifdef CONF_WINC_USE_I2C

struct i2c_master_module i2c_master_instance;
#define SLAVE_ADDRESS 0x60

/** Number of times to try to send packet if failed. */
#define I2C_TIMEOUT 100

static sint8 nm_i2c_write(uint8 *b, uint16 sz)
{
	sint8 result = M2M_SUCCESS;
	uint16_t timeout = 0;

	struct i2c_master_packet packet = {
		.address     = SLAVE_ADDRESS,
		.data_length = sz,
		.data        = b,
	};

	/* Write buffer to slave until success. */
	while (i2c_master_write_packet_wait(&i2c_master_instance, &packet) != STATUS_OK) {
		/* Increment timeout counter and check if timed out. */
		if (timeout++ == I2C_TIMEOUT) {
			break;
		}
	}

	return result;
}

static sint8 nm_i2c_read(uint8 *rb, uint16 sz)
{
	uint16_t timeout = 0;
	sint8 result = M2M_SUCCESS;
	struct i2c_master_packet packet = {
		.address     = SLAVE_ADDRESS,
		.data_length = sz,
		.data        = rb,
	};

	/* Write buffer to slave until success. */
	while (i2c_master_read_packet_wait(&i2c_master_instance, &packet) != STATUS_OK) {
		/* Increment timeout counter and check if timed out. */
		if (timeout++ == I2C_TIMEOUT) {
			break;
		}
	}

	return result;
}

static sint8 nm_i2c_write_special(uint8 *wb1, uint16 sz1, uint8 *wb2, uint16 sz2)
{
	static uint8 tmp[NM_BUS_MAX_TRX_SZ];
	m2m_memcpy(tmp, wb1, sz1);
	m2m_memcpy(&tmp[sz1], wb2, sz2);
	return nm_i2c_write(tmp, sz1+sz2);
}
#endif

#ifdef CONF_WINC_USE_SPI

struct spi_module master;
struct spi_slave_inst slave_inst;

#ifdef CONF_WINC_SPI_DMA
static volatile bool spi_dma_tx_done;
static volatile bool spi_dma_rx_done;
struct dma_resource dma_res_tx;
struct dma_resource dma_res_rx;
COMPILER_ALIGNED(32) DmacDescriptor dma_dsc_tx;
COMPILER_ALIGNED(32) DmacDescriptor dma_dsc_rx;
struct dma_descriptor_config dma_cfg_rx;
struct dma_descriptor_config dma_cfg_tx;
    
static void spi_dma_tx_completion_callback(const struct dma_resource* const resource)
{
	spi_dma_tx_done = true;
}
static void spi_dma_rx_completion_callback(const struct dma_resource* const resource)
{
	spi_dma_rx_done = true;
}

static inline sint8 spi_rw_dma(uint8* pu8Mosi, uint8* pu8Miso, uint16 u16Sz)
{
	uint32_t dummy_buf = 0;
	spi_dma_tx_done = false;
	spi_dma_rx_done = false;

	dma_cfg_tx.block_transfer_count = u16Sz;
	dma_cfg_rx.block_transfer_count = u16Sz;
	
	if (pu8Mosi) {
		dma_cfg_tx.src_increment_enable = true;
		dma_cfg_tx.source_address       = (uint32_t)pu8Mosi + u16Sz;
	} else {
		dma_cfg_tx.src_increment_enable = false;
		dma_cfg_tx.source_address       = &dummy_buf;
	}
	dma_descriptor_create(&dma_dsc_tx, &dma_cfg_tx);

	if (pu8Miso) {
		dma_cfg_rx.dst_increment_enable = true;
		dma_cfg_rx.destination_address  = (uint32_t)pu8Miso + u16Sz;
	} else {
		dma_cfg_rx.dst_increment_enable = false;
		dma_cfg_rx.destination_address  = &dummy_buf;
	}
	dma_descriptor_create(&dma_dsc_rx, &dma_cfg_rx);
	
	spi_select_slave(&master, &slave_inst, true);
	dma_start_transfer_job(&dma_res_rx);
	dma_start_transfer_job(&dma_res_tx);
    /* Must wait until both spi tx dma and spi rx dma are complete.
     * spi rx dma will be the last to complete, so could just wait on that. */
    while((!spi_dma_tx_done) || (!spi_dma_rx_done));
	spi_select_slave(&master, &slave_inst, false);
	
	return M2M_SUCCESS;
}
#endif //CONF_WINC_SPI_DMA

static inline sint8 spi_rw_pio(uint8* pu8Mosi, uint8* pu8Miso, uint16 u16Sz)
{
	uint8 u8Dummy = 0;
	uint8 u8SkipMosi = 0, u8SkipMiso = 0;
	uint16_t txd_data = 0;
	uint16_t rxd_data = 0;

	if(((pu8Miso == NULL) && (pu8Mosi == NULL)) ||(u16Sz == 0)) {
		return M2M_ERR_INVALID_ARG;
	}

	if (!pu8Mosi) {
		pu8Mosi = &u8Dummy;
		u8SkipMosi = 1;
	}
	else if(!pu8Miso) {
		pu8Miso = &u8Dummy;
		u8SkipMiso = 1;
	}
	else {
		return M2M_ERR_BUS_FAIL;
	}

	spi_select_slave(&master, &slave_inst, true);

	while (u16Sz) {
		txd_data = *pu8Mosi;
        while(!spi_is_ready_to_write(&master));
        while(spi_write(&master, txd_data) != STATUS_OK);

		/* Read SPI master data register. */
        while(!spi_is_ready_to_read(&master));
        while(spi_read(&master, &rxd_data) != STATUS_OK);
		*pu8Miso = rxd_data;
			
		u16Sz--;
		if (!u8SkipMiso)
			pu8Miso++;
		if (!u8SkipMosi)
			pu8Mosi++;
	}

	while (!spi_is_write_complete(&master))
		;

	spi_select_slave(&master, &slave_inst, false);

	return M2M_SUCCESS;
}

sint8 nm_spi_rw(uint8* pu8Mosi, uint8* pu8Miso, uint16 u16Sz)
{
#ifdef CONF_WINC_SPI_DMA
	if (u16Sz >= 8) {
		return spi_rw_dma(pu8Mosi, pu8Miso, u16Sz);
	}
	else
#endif //CONF_WINC_SPI_DMA
	{
		return spi_rw_pio(pu8Mosi, pu8Miso, u16Sz);
	}
}

#endif

/*
*	@fn		nm_bus_init
*	@brief	Initialize the bus wrapper
*	@return	M2M_SUCCESS in case of success and M2M_ERR_BUS_FAIL in case of failure
*/
sint8 nm_bus_init(void *pvinit)
{
	sint8 result = M2M_SUCCESS;

#ifdef CONF_WINC_USE_I2C
	/* Initialize config structure and software module. */
	struct i2c_master_config config_i2c_master;
	i2c_master_get_config_defaults(&config_i2c_master);

	/* Change buffer timeout to something longer. */
	config_i2c_master.buffer_timeout = 1000;

	/* Initialize and enable device with config. */
	i2c_master_init(&i2c_master_instance, SERCOM2, &config_i2c_master);

	i2c_master_enable(&i2c_master_instance);

#elif defined CONF_WINC_USE_SPI
	/* Structure for SPI configuration. */
	struct spi_config config;
	struct spi_slave_inst_config slave_config;

	/* Select SPI slave CS pin. */
	/* This step will set the CS high */
	spi_slave_inst_get_config_defaults(&slave_config);
	slave_config.ss_pin = CONF_WINC_SPI_CS_PIN;
	spi_attach_slave(&slave_inst, &slave_config);

	/* Configure the SPI master. */
	spi_get_config_defaults(&config);
	config.mux_setting = CONF_WINC_SPI_SERCOM_MUX;
	config.pinmux_pad0 = CONF_WINC_SPI_PINMUX_PAD0;
	config.pinmux_pad1 = CONF_WINC_SPI_PINMUX_PAD1;
	config.pinmux_pad2 = CONF_WINC_SPI_PINMUX_PAD2;
	config.pinmux_pad3 = CONF_WINC_SPI_PINMUX_PAD3;
	config.master_slave_select_enable = false;
	
	config.mode_specific.master.baudrate = CONF_WINC_SPI_CLOCK;
	if (spi_init(&master, CONF_WINC_SPI_MODULE, &config) != STATUS_OK) {
		return M2M_ERR_BUS_FAIL;
	}

	/* Enable the SPI master. */
	spi_enable(&master);

#ifdef CONF_WINC_SPI_DMA
	{
		struct dma_resource_config dma_config;
		spi_dma_tx_done = false;
		spi_dma_rx_done = false;
		dma_get_config_defaults(&dma_config);
		dma_config.peripheral_trigger = CONF_WINC_SPI_DMA_PERIPHERAL_TRIGGER_RX;
		dma_config.trigger_action = DMA_TRIGGER_ACTON_BEAT;
		dma_allocate(&dma_res_rx, &dma_config);
		dma_add_descriptor(&dma_res_rx, &dma_dsc_rx);
		dma_register_callback(&dma_res_rx, spi_dma_rx_completion_callback, DMA_CALLBACK_TRANSFER_DONE);
		dma_enable_callback(&dma_res_rx, DMA_CALLBACK_TRANSFER_DONE);

		dma_get_config_defaults(&dma_config);
		dma_config.peripheral_trigger = CONF_WINC_SPI_DMA_PERIPHERAL_TRIGGER_TX;
		dma_config.trigger_action = DMA_TRIGGER_ACTON_BEAT;
		dma_allocate(&dma_res_tx, &dma_config);
		dma_add_descriptor(&dma_res_tx, &dma_dsc_tx);
		dma_register_callback(&dma_res_tx, spi_dma_tx_completion_callback, DMA_CALLBACK_TRANSFER_DONE);
		dma_enable_callback(&dma_res_tx, DMA_CALLBACK_TRANSFER_DONE);

		dma_descriptor_get_config_defaults(&dma_cfg_rx);
		dma_descriptor_get_config_defaults(&dma_cfg_tx);
		dma_cfg_tx.destination_address  = (uint32_t)(&master.hw->SPI.DATA.reg);
		dma_cfg_tx.dst_increment_enable = false;
		dma_cfg_rx.source_address       = (uint32_t)(&master.hw->SPI.DATA.reg);
		dma_cfg_rx.src_increment_enable = false;
	}
#endif

	nm_bsp_reset();
	nm_bsp_sleep(1);
#endif
	return result;
}

/*
*	@fn		nm_bus_ioctl
*	@brief	send/receive from the bus
*	@param[IN]	u8Cmd
*					IOCTL command for the operation
*	@param[IN]	pvParameter
*					Arbitrary parameter depending on IOCTL
*	@return	M2M_SUCCESS in case of success and M2M_ERR_BUS_FAIL in case of failure
*	@note	For SPI only, it's important to be able to send/receive at the same time
*/
sint8 nm_bus_ioctl(uint8 u8Cmd, void* pvParameter)
{
	sint8 s8Ret = 0;
	switch(u8Cmd)
	{
#ifdef CONF_WINC_USE_I2C
		case NM_BUS_IOCTL_R: {
			tstrNmI2cDefault *pstrParam = (tstrNmI2cDefault *)pvParameter;
			s8Ret = nm_i2c_read(pstrParam->pu8Buf, pstrParam->u16Sz);
		}
		break;
		case NM_BUS_IOCTL_W: {
			tstrNmI2cDefault *pstrParam = (tstrNmI2cDefault *)pvParameter;
			s8Ret = nm_i2c_write(pstrParam->pu8Buf, pstrParam->u16Sz);
		}
		break;
		case NM_BUS_IOCTL_W_SPECIAL: {
			tstrNmI2cSpecial *pstrParam = (tstrNmI2cSpecial *)pvParameter;
			s8Ret = nm_i2c_write_special(pstrParam->pu8Buf1, pstrParam->u16Sz1, pstrParam->pu8Buf2, pstrParam->u16Sz2);
		}
		break;
#elif defined CONF_WINC_USE_SPI
		case NM_BUS_IOCTL_RW: {
			tstrNmSpiRw *pstrParam = (tstrNmSpiRw *)pvParameter;
			s8Ret = nm_spi_rw(pstrParam->pu8InBuf, pstrParam->pu8OutBuf, pstrParam->u16Sz);
		}
		break;
#endif
		default:
			s8Ret = -1;
			M2M_ERR("invalid ioclt cmd\n");
			break;
	}

	return s8Ret;
}

/*
*	@fn		nm_bus_deinit
*	@brief	De-initialize the bus wrapper
*/
sint8 nm_bus_deinit(void)
{
	sint8 result = M2M_SUCCESS;
	struct port_config pin_conf;
		
	port_get_config_defaults(&pin_conf);
	/* Configure control pins as input no pull up. */
	pin_conf.direction  = PORT_PIN_DIR_INPUT;
	pin_conf.input_pull = PORT_PIN_PULL_NONE;

#ifdef CONF_WINC_USE_I2C
	i2c_master_disable(&i2c_master_instance);
	port_pin_set_config(CONF_WINC_I2C_SCL, &pin_conf);
	port_pin_set_config(CONF_WINC_I2C_SDA, &pin_conf);
#endif /* CONF_WINC_USE_I2C */
#ifdef CONF_WINC_USE_SPI
	nm_bsp_deinit();

	spi_disable(&master);
	port_pin_set_config(CONF_WINC_SPI_MOSI, &pin_conf);
	port_pin_set_config(CONF_WINC_SPI_MISO, &pin_conf);
	port_pin_set_config(CONF_WINC_SPI_SCK,  &pin_conf);
	port_pin_set_config(CONF_WINC_SPI_SS,   &pin_conf);
	
#ifdef CONF_WINC_SPI_DMA
    dma_free(&dma_res_tx);
    dma_free(&dma_res_rx);
#endif //CONF_WINC_SPI_DMA
	//port_pin_set_output_level(CONF_WINC_SPI_MOSI, false);
	//port_pin_set_output_level(CONF_WINC_SPI_MISO, false);
	//port_pin_set_output_level(CONF_WINC_SPI_SCK,  false);
	//port_pin_set_output_level(CONF_WINC_SPI_SS,   false);
#endif /* CONF_WINC_USE_SPI */
	return result;
}

/*
*	@fn			nm_bus_reinit
*	@brief		re-initialize the bus wrapper
*	@param [in]	void *config
*					re-init configuration data
*	@return		M2M_SUCCESS in case of success and M2M_ERR_BUS_FAIL in case of failure
*/
sint8 nm_bus_reinit(void* config)
{
	return M2M_SUCCESS;
}

