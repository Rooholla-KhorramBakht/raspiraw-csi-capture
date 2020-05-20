/*
Copyright (c) 2015, Raspberry Pi Foundation
Copyright (c) 2015, Dave Stevenson
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#define VERSION_STRING "0.0.2"

#define _GNU_SOURCE
#include <ctype.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include "interface/vcos/vcos.h"
#include "bcm_host.h"

#include "interface/mmal/mmal.h"
#include "interface/mmal/mmal_buffer.h"
#include "interface/mmal/mmal_logging.h"
#include "interface/mmal/util/mmal_default_components.h"
#include "interface/mmal/util/mmal_util.h"
#include "interface/mmal/util/mmal_util_params.h"
#include "interface/mmal/util/mmal_connection.h"

#include "RaspiCLI.h"

#include <sys/ioctl.h>

#include "raw_header.h"

struct brcm_raw_header *brcm_header = NULL;

struct mode_def
{
	int width;
	int height;
	MMAL_FOURCC_T encoding;
	int native_bit_depth;
	uint8_t image_id;
	uint8_t data_lanes;
	unsigned int min_vts;
	int line_time_ns;
	uint32_t timing1;
	uint32_t timing2;
	uint32_t timing3;
	uint32_t timing4;
	uint32_t timing5;
	uint32_t term1;
	uint32_t term2;
	int black_level;
};

#define NUM_MODES 			1
#define CSI_CHANNEL_NUM 1
struct mode_def u96_modes[] = {

   {  1280,  720, MMAL_ENCODING_BGR24, 8, 0x24, 2,  484,  23276,
      0, 0, 0, 0, 0, 0, 0, 16 },
};
// Command List
enum {
	CommandHelp,
	CommandMode,
	CommandOutput,
	CommandWriteHeader,
	CommandTimeout,
	CommandSaveRate,
	CommandWidth,
	CommandHeight,
	CommandWriteHeader0,
	CommandWriteHeaderG,
	CommandWriteTimestamps,
	CommandWriteEmpty,
};

static COMMAND_LIST cmdline_commands[] =
{
	{ CommandHelp,		"-help",	"?",  "This help information", 0 },
	{ CommandMode,		"-mode",	"md", "Set sensor mode <mode>", 1 },
	{ CommandOutput,	"-output",	"o",  "Set the output filename", 0 },
	{ CommandWriteHeader,	"-header",	"hd", "Write the BRCM header to the output file", 0 },
	{ CommandSaveRate, 	"-saverate",	"sr", "Save every Nth frame", 1 },
	{ CommandWidth,		"-width",	"w",  "Set current mode width", -1},
	{ CommandHeight,	"-height",	"h",  "Set current mode height", -1},
	{ CommandWriteHeader0,	"-header0",	"hd0","Sets filename to write the BRCM header to", 0 },
	{ CommandWriteHeaderG,	"-headerg",	"hdg","Sets filename to write the .pgm header to", 0 },
	{ CommandWriteTimestamps,"-tstamps",	"ts", "Sets filename to write timestamps to", 0 },
	{ CommandWriteEmpty,	"-empty",	"emp","Write empty output files", 0 },
	{ CommandTimeout,	"-timeout",	"t",  "Time (in ms) before shutting down (if not specified, set to 5s)", 1 },
};

static int cmdline_commands_size = sizeof(cmdline_commands) / sizeof(cmdline_commands[0]);

typedef struct pts_node {
	int	idx;
	int64_t  pts;
	struct pts_node *nxt;
} *PTS_NODE_T;

typedef struct {
	int mode;
	char *output;
	int capture;
	int write_header;
	int timeout;
	int saverate;
	char *regs;
	int width;
	int height;
	char *write_header0;
	char *write_headerg;
	char *write_timestamps;
	int write_empty;
        PTS_NODE_T ptsa;
        PTS_NODE_T ptso;
} RASPIRAW_PARAMS_T;

/**
 * Allocates and generates a filename based on the
 * user-supplied pattern and the frame number.
 * On successful return, finalName and tempName point to malloc()ed strings
 * which must be freed externally.  (On failure, returns nulls that
 * don't need free()ing.)
 *
 * @param finalName pointer receives an
 * @param pattern sprintf pattern with %d to be replaced by frame
 * @param frame for timelapse, the frame number
 * @return Returns a MMAL_STATUS_T giving result of operation
*/

MMAL_STATUS_T create_filenames(char** finalName, char * pattern, int frame)
{
	*finalName = NULL;
	if (0 > asprintf(finalName, pattern, frame))
	{
		return MMAL_ENOMEM;    // It may be some other error, but it is not worth getting it right
	}
	return MMAL_SUCCESS;
}

int running = 0;
static void callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
	static int count = 0;
	vcos_log_error("Buffer %p returned, filled %d, timestamp %llu, flags %04X", buffer, buffer->length, buffer->pts, buffer->flags);
	if (running)
	{
		RASPIRAW_PARAMS_T *cfg = (RASPIRAW_PARAMS_T *)port->userdata;

		if (!(buffer->flags&MMAL_BUFFER_HEADER_FLAG_CODECSIDEINFO) &&
                    (((count++)%cfg->saverate)==0))
		{
			// Save every Nth frame
			// SD card access is too slow to do much more.
			FILE *file;
			char *filename = NULL;
			if (create_filenames(&filename, cfg->output, count) == MMAL_SUCCESS)
			{
				file = fopen(filename, "wb");
				if (file)
				{
					if (cfg->ptso)  // make sure previous malloc() was successful
					{
						cfg->ptso->idx = count;
						cfg->ptso->pts = buffer->pts;
						cfg->ptso->nxt = malloc(sizeof(*cfg->ptso->nxt));
						cfg->ptso = cfg->ptso->nxt;
					}
					if (!cfg->write_empty)
					{
						if (cfg->write_header)
							fwrite(brcm_header, BRCM_RAW_HEADER_LENGTH, 1, file);
						fwrite(buffer->data, buffer->length, 1, file);
					}
					fclose(file);
				}
				free(filename);
			}
		}
		buffer->length = 0;
		mmal_port_send_buffer(port, buffer);
	}
	else
		mmal_buffer_header_release(buffer);
}

/**
 * Parse the incoming command line and put resulting parameters in to the state
 *
 * @param argc Number of arguments in command line
 * @param argv Array of pointers to strings from command line
 * @param state Pointer to state structure to assign any discovered parameters to
 * @return non-0 if failed for some reason, 0 otherwise
 */
static int parse_cmdline(int argc, char **argv, RASPIRAW_PARAMS_T *cfg)
{
	// Parse the command line arguments.
	// We are looking for --<something> or -<abbreviation of something>

	int valid = 1;
	int i;

	for (i = 1; i < argc && valid; i++)
	{
		int command_id, num_parameters, len;

		if (!argv[i])
			continue;

		if (argv[i][0] != '-')
		{
			valid = 0;
			continue;
		}

		// Assume parameter is valid until proven otherwise
		valid = 1;

		command_id = raspicli_get_command_id(cmdline_commands, cmdline_commands_size, &argv[i][1], &num_parameters);

		// If we found a command but are missing a parameter, continue (and we will drop out of the loop)
		if (command_id != -1 && num_parameters > 0 && (i + 1 >= argc) )
			continue;

		//  We are now dealing with a command line option
		switch (command_id)
		{
			case CommandHelp:
				raspicli_display_help(cmdline_commands, cmdline_commands_size);
				// exit straight away if help requested
				return -1;

			case CommandMode:
				if (sscanf(argv[i + 1], "%d", &cfg->mode) != 1)
					valid = 0;
				else
					i++;
				break;

			case CommandOutput:  // output filename
			{
				len = strlen(argv[i + 1]);
				if (len)
				{
					//We use sprintf to append the frame number for timelapse mode
					//Ensure that any %<char> is either %% or %d.
					const char *percent = argv[i+1];
					while(valid && *percent && (percent=strchr(percent, '%')) != NULL)
					{
					int digits=0;
					percent++;
					while(isdigit(*percent))
					{
						percent++;
						digits++;
					}
					if (!((*percent == '%' && !digits) || *percent == 'd'))
					{
						valid = 0;
						fprintf(stderr, "Filename contains %% characters, but not %%d or %%%% - sorry, will fail\n");
					}
					percent++;
				}
				cfg->output = malloc(len + 10); // leave enough space for any timelapse generated changes to filename
				vcos_assert(cfg->output);
				if (cfg->output)
					strncpy(cfg->output, argv[i + 1], len+1);
					i++;
					cfg->capture = 1;
				}
				else
					valid = 0;
				break;
			}

			case CommandWriteHeader:
				cfg->write_header = 1;
				break;

			case CommandTimeout: // Time to run for in milliseconds
				if (sscanf(argv[i + 1], "%u", &cfg->timeout) == 1)
				{
					i++;
				}
				else
					valid = 0;
				break;

			case CommandSaveRate:
				if (sscanf(argv[i + 1], "%u", &cfg->saverate) == 1)
				{
					i++;
				}
				else
					valid = 0;
				break;

			case CommandWidth:
				if (sscanf(argv[i + 1], "%d", &cfg->width) != 1)
					valid = 0;
				else
					i++;
				break;

			case CommandHeight:
				if (sscanf(argv[i + 1], "%d", &cfg->height) != 1)
					valid = 0;
				else
					i++;
				break;

			case CommandWriteHeader0:
				len = strlen(argv[i + 1]);
				cfg->write_header0 = malloc(len + 1);
				vcos_assert(cfg->write_header0);
				strncpy(cfg->write_header0, argv[i + 1], len+1);
				i++;
				break;

			case CommandWriteHeaderG:
				len = strlen(argv[i + 1]);
				cfg->write_headerg = malloc(len + 1);
				vcos_assert(cfg->write_headerg);
				strncpy(cfg->write_headerg, argv[i + 1], len+1);
				i++;
				break;

			case CommandWriteTimestamps:
				len = strlen(argv[i + 1]);
				cfg->write_timestamps = malloc(len + 1);
				vcos_assert(cfg->write_timestamps);
				strncpy(cfg->write_timestamps, argv[i + 1], len+1);
				i++;
				cfg->ptsa = malloc(sizeof(*cfg->ptsa));
				cfg->ptso = cfg->ptsa;
				break;

			case CommandWriteEmpty:
				cfg->write_empty = 1;
				break;

			default:
				valid = 0;
				break;
		}
	}

	if (!valid)
	{
		fprintf(stderr, "Invalid command line option (%s)\n", argv[i-1]);
		return 1;
	}

	return 0;
}
int main(int argc, char** argv) {
	RASPIRAW_PARAMS_T cfg = {
		.mode = 0,
		.output = NULL,
		.capture = 0,
		.write_header = 0,
		.timeout = 5000,
		.saverate = 20,
		.width = -1,
		.height = -1,
		.write_header0 = NULL,
		.write_headerg = NULL,
		.write_timestamps = NULL,
		.write_empty = 0,
		.ptsa = NULL,
		.ptso = NULL,
	};
	uint32_t encoding;
	const struct sensor_def *sensor;
	struct mode_def *sensor_mode = NULL;

	bcm_host_init();
	vcos_log_register("RaspiRaw", VCOS_LOG_CATEGORY);

	if (argc == 1)
	{
		fprintf(stdout, "\n%s Camera App %s\n\n", basename(argv[0]), VERSION_STRING);

		raspicli_display_help(cmdline_commands, cmdline_commands_size);
		exit(-1);
	}

	// Parse the command line and put options in to our status structure

  if (parse_cmdline(argc, argv, &cfg))
	{
		exit(-1);
	}

	if (cfg.mode >= 0 && cfg.mode < NUM_MODES)
	{
		sensor_mode = &u96_modes[cfg.mode];
	}

	if (!sensor_mode)
	{
		vcos_log_error("Invalid mode %d - aborting", cfg.mode);
		return -2;
	}
	if (cfg.width > 0)
	{
	        sensor_mode->width = cfg.width;
	}

	if (cfg.height > 0)
	{
		sensor_mode->height = cfg.height;
	}

	if (sensor_mode->encoding == 0)
		encoding = MMAL_ENCODING_BGR24;
	else
		encoding = sensor_mode->encoding;
	vcos_log_error("Encoding %08X", encoding);

	MMAL_COMPONENT_T *rawcam=NULL, *isp=NULL, *render=NULL;
	MMAL_STATUS_T status;
	MMAL_PORT_T *output = NULL;
	MMAL_POOL_T *pool = NULL;
	MMAL_CONNECTION_T *rawcam_isp = NULL;
	MMAL_CONNECTION_T *isp_render = NULL;
	MMAL_PARAMETER_CAMERA_RX_CONFIG_T rx_cfg = {{MMAL_PARAMETER_CAMERA_RX_CONFIG, sizeof(rx_cfg)}};
	MMAL_PARAMETER_CAMERA_RX_TIMING_T rx_timing = {{MMAL_PARAMETER_CAMERA_RX_TIMING, sizeof(rx_timing)}};
	int i;

	bcm_host_init();
	vcos_log_register("RaspiRaw", VCOS_LOG_CATEGORY);

	status = mmal_component_create("vc.ril.rawcam", &rawcam);
	if (status != MMAL_SUCCESS)
	{
		vcos_log_error("Failed to create rawcam");
		return -1;
	}

	status = mmal_component_create("vc.ril.isp", &isp);
	if (status != MMAL_SUCCESS)
	{
		vcos_log_error("Failed to create isp");
		goto component_destroy;
	}

	status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_RENDERER, &render);
	if (status != MMAL_SUCCESS)
	{
		vcos_log_error("Failed to create render");
		goto component_destroy;
	}

	output = rawcam->output[0];
	status = mmal_port_parameter_get(output, &rx_cfg.hdr);
	if (status != MMAL_SUCCESS)
	{
		vcos_log_error("Failed to get cfg");
		goto component_destroy;
	}

	rx_cfg.unpack = MMAL_CAMERA_RX_CONFIG_UNPACK_NONE;
	rx_cfg.pack = MMAL_CAMERA_RX_CONFIG_PACK_NONE;
	vcos_log_error("Set pack to %d, unpack to %d", rx_cfg.unpack, rx_cfg.pack);

	if (sensor_mode->data_lanes)
		rx_cfg.data_lanes = sensor_mode->data_lanes;
	if (sensor_mode->image_id)
		rx_cfg.image_id = sensor_mode->image_id;
	rx_cfg.embedded_data_lines = 128;

	status = mmal_port_parameter_set(output, &rx_cfg.hdr);
	if (status != MMAL_SUCCESS)
	{
		vcos_log_error("Failed to set cfg");
		goto component_destroy;
	}
	status = mmal_port_parameter_get(output, &rx_timing.hdr);
	if (status != MMAL_SUCCESS)
	{
		vcos_log_error("Failed to get timing");
		goto component_destroy;
	}
	// Take a look at here
	if (sensor_mode->timing1)
		rx_timing.timing1 = sensor_mode->timing1;
	if (sensor_mode->timing2)
		rx_timing.timing2 = sensor_mode->timing2;
	if (sensor_mode->timing3)
		rx_timing.timing3 = sensor_mode->timing3;
	if (sensor_mode->timing4)
		rx_timing.timing4 = sensor_mode->timing4;
	if (sensor_mode->timing5)
		rx_timing.timing5 = sensor_mode->timing5;
	if (sensor_mode->term1)
		rx_timing.term1 = 0;//sensor_mode->term1; //Clock Lane Manual Tenmination
	if (sensor_mode->term2)
		rx_timing.term2 = 0;//sensor_mode->term2; //Data Lane Manual Tenmination

	vcos_log_error("Timing %u/%u, %u/%u/%u, %u/%u",
		rx_timing.timing1, rx_timing.timing2,
		rx_timing.timing3, rx_timing.timing4, rx_timing.timing5,
		rx_timing.term1,  rx_timing.term2);
	status = mmal_port_parameter_set(output, &rx_timing.hdr);
	if (status != MMAL_SUCCESS)
	{
		vcos_log_error("Failed to set timing");
		goto component_destroy;
	}

	vcos_log_error("Set camera_num to %d", 0);
	status = mmal_port_parameter_set_int32(output, MMAL_PARAMETER_CAMERA_NUM, CSI_CHANNEL_NUM);
	if (status != MMAL_SUCCESS)
	{
		vcos_log_error("Failed to set camera_num");
		goto component_destroy;
	}

	status = mmal_component_enable(rawcam);
	if (status != MMAL_SUCCESS)
	{
		vcos_log_error("Failed to enable rawcam");
		goto component_destroy;
	}
	status = mmal_component_enable(isp);
	if (status != MMAL_SUCCESS)
	{
		vcos_log_error("Failed to enable isp");
		goto component_destroy;
	}
	status = mmal_component_enable(render);
	if (status != MMAL_SUCCESS)
	{
		vcos_log_error("Failed to enable render");
		goto component_destroy;
	}

	output->format->es->video.crop.width = sensor_mode->width;
	output->format->es->video.crop.height = sensor_mode->height;
	output->format->es->video.width = VCOS_ALIGN_UP(sensor_mode->width, 16);
	output->format->es->video.height = VCOS_ALIGN_UP(sensor_mode->height, 16);
	output->format->encoding = encoding;

	status = mmal_port_format_commit(output);
	if (status != MMAL_SUCCESS)
	{
		vcos_log_error("Failed port_format_commit");
		goto component_disable;
	}

	output->buffer_size = output->buffer_size_recommended;
	output->buffer_num = output->buffer_num_recommended;

	if (cfg.capture)
	{
		if (cfg.write_header || cfg.write_header0)
		{
			brcm_header = (struct brcm_raw_header*)malloc(BRCM_RAW_HEADER_LENGTH);
			if (brcm_header)
			{
				memset(brcm_header, 0, BRCM_RAW_HEADER_LENGTH);
				brcm_header->id = BRCM_ID_SIG;
				brcm_header->version = HEADER_VERSION;
				brcm_header->mode.width = sensor_mode->width;
				brcm_header->mode.height = sensor_mode->height;

				brcm_header->mode.format = VC_IMAGE_BGR888;

				if (cfg.write_header0)
				{
					// Save bcrm_header into one file only
					FILE *file;
					file = fopen(cfg.write_header0, "wb");
					if (file)
					{
						fwrite(brcm_header, BRCM_RAW_HEADER_LENGTH, 1, file);
						fclose(file);
					}
				}
			}
		}
		else if (cfg.write_headerg)
		{
			// Save pgm_header into one file only
			FILE *file;
			file = fopen(cfg.write_headerg, "wb");
			if (file)
			{
				fprintf(file, "P5\n%d %d\n255\n", sensor_mode->width, sensor_mode->height);
				fclose(file);
			}
		}

		status = mmal_port_parameter_set_boolean(output, MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
		if (status != MMAL_SUCCESS)
		{
			vcos_log_error("Failed to set zero copy");
			goto component_disable;
		}

		vcos_log_error("Create pool of %d buffers of size %d", output->buffer_num, output->buffer_size);
		pool = mmal_port_pool_create(output, output->buffer_num, output->buffer_size);
		if (!pool)
		{
			vcos_log_error("Failed to create pool");
			goto component_disable;
		}

		output->userdata = (struct MMAL_PORT_USERDATA_T *)&cfg;
		status = mmal_port_enable(output, callback);
		if (status != MMAL_SUCCESS)
		{
			vcos_log_error("Failed to enable port");
			goto pool_destroy;
		}
		running = 1;
		for(i = 0; i<output->buffer_num; i++)
		{
			MMAL_BUFFER_HEADER_T *buffer = mmal_queue_get(pool->queue);

			if (!buffer)
			{
				vcos_log_error("Where'd my buffer go?!");
				goto port_disable;
			}
			status = mmal_port_send_buffer(output, buffer);
			if (status != MMAL_SUCCESS)
			{
				vcos_log_error("mmal_port_send_buffer failed on buffer %p, status %d", buffer, status);
				goto port_disable;
			}
			vcos_log_error("Sent buffer %p", buffer);
		}
	}
	else
	{
		status = mmal_connection_create(&rawcam_isp, output, isp->input[0], MMAL_CONNECTION_FLAG_TUNNELLING);
		if (status != MMAL_SUCCESS)
		{
			vcos_log_error("Failed to create rawcam->isp connection");
			goto pool_destroy;
		}

		MMAL_PORT_T *port = isp->output[0];
		port->format->es->video.crop.width = sensor_mode->width;
		port->format->es->video.crop.height = sensor_mode->height;
		if (port->format->es->video.crop.width > 1920)
		{
			//Display can only go up to a certain resolution before underflowing
			port->format->es->video.crop.width /= 2;
			port->format->es->video.crop.height /= 2;
		}
		port->format->es->video.width = VCOS_ALIGN_UP(port->format->es->video.crop.width, 32);
		port->format->es->video.height = VCOS_ALIGN_UP(port->format->es->video.crop.height, 16);
		port->format->encoding = MMAL_ENCODING_I420;
		status = mmal_port_format_commit(port);
		if (status != MMAL_SUCCESS)
		{
			vcos_log_error("Failed to commit port format on isp output");
			goto pool_destroy;
		}

		if (sensor_mode->black_level)
		{
			status = mmal_port_parameter_set_uint32(isp->input[0], MMAL_PARAMETER_BLACK_LEVEL, sensor_mode->black_level);
			if (status != MMAL_SUCCESS)
			{
				vcos_log_error("Failed to set black level - try updating firmware");
			}
		}

		// if (cfg.awb_gains_r && cfg.awb_gains_b)
		// {
		// 	MMAL_PARAMETER_AWB_GAINS_T param = {{MMAL_PARAMETER_CUSTOM_AWB_GAINS,sizeof(param)}, {0,0}, {0,0}};
		//
		// 	param.r_gain.num = (unsigned int)(cfg.awb_gains_r * 65536);
		// 	param.b_gain.num = (unsigned int)(cfg.awb_gains_b * 65536);
		// 	param.r_gain.den = param.b_gain.den = 65536;
		// 	status = mmal_port_parameter_set(isp->input[0], &param.hdr);
		// 	if (status != MMAL_SUCCESS)
		// 	{
		// 		vcos_log_error("Failed to set white balance");
		// 	}
		// }

		status = mmal_connection_create(&isp_render, isp->output[0], render->input[0], MMAL_CONNECTION_FLAG_TUNNELLING);
		if (status != MMAL_SUCCESS)
		{
			vcos_log_error("Failed to create isp->render connection");
			goto pool_destroy;
		}

		status = mmal_connection_enable(rawcam_isp);
		if (status != MMAL_SUCCESS)
		{
			vcos_log_error("Failed to enable rawcam->isp connection");
			goto pool_destroy;
		}
		status = mmal_connection_enable(isp_render);
		if (status != MMAL_SUCCESS)
		{
			vcos_log_error("Failed to enable isp->render connection");
			goto pool_destroy;
		}
	}

	//start_camera_streaming(sensor, sensor_mode);

	vcos_sleep(cfg.timeout);
	running = 0;

	//stop_camera_streaming(sensor);

port_disable:
	if (cfg.capture)
	{
		status = mmal_port_disable(output);
		if (status != MMAL_SUCCESS)
		{
			vcos_log_error("Failed to disable port");
			return -1;
		}
	}
pool_destroy:
	if (pool)
		mmal_port_pool_destroy(output, pool);
	if (isp_render)
	{
		mmal_connection_disable(isp_render);
		mmal_connection_destroy(isp_render);
	}
	if (rawcam_isp)
	{
		mmal_connection_disable(rawcam_isp);
		mmal_connection_destroy(rawcam_isp);
	}
component_disable:
	if (brcm_header)
		free(brcm_header);
	status = mmal_component_disable(render);
	if (status != MMAL_SUCCESS)
	{
		vcos_log_error("Failed to disable render");
	}
	status = mmal_component_disable(isp);
	if (status != MMAL_SUCCESS)
	{
		vcos_log_error("Failed to disable isp");
	}
	status = mmal_component_disable(rawcam);
	if (status != MMAL_SUCCESS)
	{
		vcos_log_error("Failed to disable rawcam");
	}
component_destroy:
	if (rawcam)
		mmal_component_destroy(rawcam);
	if (isp)
		mmal_component_destroy(isp);
	if (render)
		mmal_component_destroy(render);

	if (cfg.write_timestamps)
	{
		// Save timestamps
		FILE *file;
		file = fopen(cfg.write_timestamps, "wb");
		if (file)
		{
			int64_t old;
			PTS_NODE_T aux;
			for(aux = cfg.ptsa; aux != cfg.ptso; aux = aux->nxt)
			{
				if (aux == cfg.ptsa)
				{
					fprintf(file, ",%d,%lld\n", aux->idx, aux->pts);
				}
				else
				{
					fprintf(file, "%lld,%d,%lld\n", aux->pts-old, aux->idx, aux->pts);
				}
				old = aux->pts;
			}
			fclose(file);
		}

		while (cfg.ptsa != cfg.ptso)
		{
			PTS_NODE_T aux = cfg.ptsa->nxt;
			free(cfg.ptsa);
			cfg.ptsa = aux;
		}
		free(cfg.ptso);
	}

	return 0;
}
