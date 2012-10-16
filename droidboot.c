/*
 * Copyright 2011 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <cmfwdl.h>
#include <droidboot.h>
#include <droidboot_plugin.h>
#include <droidboot_util.h>
#include <fcntl.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cutils/properties.h>
#include <cutils/android_reboot.h>
#include <unistd.h>

#include "volumeutils/ufdisk.h"
#include "update_osip.h"
#include "util.h"
#include "modem_fw.h"
#include "modem_nvm.h"
#include "fw_version_check.h"
#include "flash_ifwi.h"
#include "fastboot.h"
#include "droidboot_ui.h"
#include "update_partition.h"

#define IMG_RADIO "/radio.img"
#define IMG_RADIO_RND "/radio_rnd.img"

static void progress_callback(enum cmfwdl_status_type type, int value,
		const char *msg, void *data)
{
	static int last_update_progress = -1;

	switch (type) {
	case cmfwdl_status_booting:
		pr_debug("modem: Booting...");
		last_update_progress = -1;
		break;
	case cmfwdl_status_synced:
		pr_info("modem: Device Synchronized");
		last_update_progress = -1;
		break;
	case cmfwdl_status_downloading:
		pr_info("modem: Loading Component %s", msg);
		last_update_progress = -1;
		break;
	case cmfwdl_status_msg_detail:
		pr_info("modem: <msg> %s", msg);
		last_update_progress = -1;
		break;
	case cmfwdl_status_error_detail:
		pr_error("modem: ERROR: %s", msg);
		last_update_progress = -1;
		break;
	case cmfwdl_status_progress:
		pr_info("    <Progress> %d%%", value);
		last_update_progress = value;
		break;
	case cmfwdl_status_version:
		pr_info("modem: Version: %s", msg);
		break;
	default:
		pr_info("modem: Ignoring: %s", msg);
		break;
	}
}

static void nvm_output_callback(const char *msg, int output)
{
	if (output & OUTPUT_DEBUG) {
		pr_debug(msg);
	}
	if (output & OUTPUT_FASTBOOT_INFO) {
		fastboot_info(msg);
	}
}

static int flash_image(void *data, unsigned sz, int index)
{
	if (index < 0) {
		pr_error("Can't find OSII index!!");
		return -1;
	}
	return write_stitch_image(data, sz, index);
}

static int flash_android_kernel(void *data, unsigned sz)
{
	return flash_image(data, sz, get_named_osii_index(ANDROID_OS_NAME));
}

static int flash_recovery_kernel(void *data, unsigned sz)
{
	return flash_image(data, sz, get_named_osii_index(RECOVERY_OS_NAME));
}

static int flash_fastboot_kernel(void *data, unsigned sz)
{
	return flash_image(data, sz, get_named_osii_index(FASTBOOT_OS_NAME));
}

static int flash_splashscreen_image(void *data, unsigned sz)
{
	return flash_image(data, sz, get_attribute_osii_index(ATTR_SIGNED_SPLASHSCREEN));
}

static int flash_uefi_firmware(void *data, unsigned sz)
{
	return flash_image(data, sz, get_named_osii_index(UEFI_FW_NAME));
}

static int flash_modem(void *data, unsigned sz)
{
	int ret;
	int argc = 1;
	char *argv[1];

	if (file_write(IMG_RADIO, data, sz)) {
		pr_error("Couldn't write radio image to %s", IMG_RADIO);
		return -1;
	}
	argv[0] = "f";
	/* Update modem SW. */
	ret = flash_modem_fw(IMG_RADIO, IMG_RADIO, argc, argv, progress_callback);
	unlink(IMG_RADIO);
	return ret;
}

static int flash_modem_no_end_reboot(void *data, unsigned sz)
{
	int ret;
	int argc = 1;
	char *argv[1];

	if (file_write(IMG_RADIO, data, sz)) {
		pr_error("Couldn't write radio image to %s", IMG_RADIO);
		return -1;
	}
	argv[0] = "d";
	/* Update modem SW. */
	ret = flash_modem_fw(IMG_RADIO, IMG_RADIO, argc, argv, progress_callback);
	unlink(IMG_RADIO);
	return ret;
}

static int flash_modem_get_fuse(void *data, unsigned sz)
{
	int ret;
	int argc = 1;
	char *argv[1];

	if (file_write(IMG_RADIO, data, sz)) {
		pr_error("Couldn't write radio image to %s", IMG_RADIO);
		return -1;
	}
	argv[0] = "u"; /* Update modem SW and get chip fusing parameters */
	ret = flash_modem_fw(IMG_RADIO, IMG_RADIO, argc, argv, progress_callback);
	unlink(IMG_RADIO);
	return ret;
}

static int flash_modem_get_fuse_only(void *data, unsigned sz)
{
	int ret;
	int argc = 1;
	char *argv[1];

	if (file_write(IMG_RADIO, data, sz)) {
		pr_error("Couldn't write radio image to %s", IMG_RADIO);
		return -1;
	}

	argv[0] = "v"; /* Only get chip fusing parameters */
	ret = flash_modem_fw(IMG_RADIO, IMG_RADIO, argc, argv, progress_callback);
	unlink(IMG_RADIO);
	return ret;
}

static int flash_modem_erase_all(void *data, unsigned sz)
{
	int ret;
	int argc = 2;
	char *argv[2];

	if (file_write(IMG_RADIO, data, sz)) {
		pr_error("Couldn't write radio image to %s", IMG_RADIO);
		return -1;
	}
	argv[0] = "e";
	argv[1] = "f";
	/* Update modem SW. */
	ret = flash_modem_fw(IMG_RADIO, IMG_RADIO, argc, argv, progress_callback);
	unlink(IMG_RADIO);
	return ret;
}

static int flash_modem_store_fw(void *data, unsigned sz)
{
	/* Save locally modem SW (to be called first before flashing RND Cert) */
	if (file_write(IMG_RADIO, data, sz)) {
		pr_error("Couldn't write radio image to %s", IMG_RADIO);
		return -1;
	}
	printf("Radio Image Saved\n");
	return 0;
}

static int flash_modem_read_rnd(void *data, unsigned sz)
{
	int ret;
	int argc = 1;
	char *argv[1];

	if (file_write(IMG_RADIO, data, sz)) {
		pr_error("Couldn't write modem fw to %s", IMG_RADIO);
		return -1;
	}
	argv[0] = "g";
	/* Get RND Cert (print out in stdout) */
	ret = flash_modem_fw(IMG_RADIO, NULL, argc, argv, progress_callback);
	unlink(IMG_RADIO);
	return ret;
}

static int flash_modem_write_rnd(void *data, unsigned sz)
{
	int ret;
	int argc = 1;
	char *argv[1];

	if (access(IMG_RADIO, F_OK)) {
		pr_error("Radio Image %s Not Found!!\nCall flash radio_img first", IMG_RADIO);
		return -1;
	}
	if (file_write(IMG_RADIO_RND, data, sz)) {
		pr_error("Couldn't write radio_rnd image to %s", IMG_RADIO_RND);
		return -1;
	}
	argv[0] = "r";
	/* Flash RND Cert */
	ret = flash_modem_fw(IMG_RADIO, IMG_RADIO_RND, argc, argv, progress_callback);
	unlink(IMG_RADIO);
	unlink(IMG_RADIO_RND);
	return ret;
}

static int flash_modem_erase_rnd(void *data, unsigned sz)
{
	int ret;
	int argc = 1;
	char *argv[1];

	if (file_write(IMG_RADIO, data, sz)) {
		pr_error("Couldn't write radio image to %s", IMG_RADIO);
		return -1;
	}
	argv[0] = "y";
	/* Erase RND Cert */
	ret = flash_modem_fw(IMG_RADIO, NULL, argc, argv, progress_callback);
	unlink(IMG_RADIO);
	return ret;
}

static int flash_modem_get_hw_id(void *data, unsigned sz)
{
	int ret;
	int argc = 1;
	char *argv[1];

	if (file_write(IMG_RADIO, data, sz)) {
		pr_error("Couldn't write radio image to %s", IMG_RADIO);
		return -1;
	}
	printf("Getting radio HWID...\n");
	argv[0] = "h";
	/* Get modem HWID (print out in stdout) */
	ret = flash_modem_fw(IMG_RADIO, NULL, argc, argv, progress_callback);
	unlink(IMG_RADIO);
	return ret;
}

#define BIN_DNX  "/tmp/__dnx.bin"
#define BIN_IFWI "/tmp/__ifwi.bin"

static int flash_dnx(void *data, unsigned sz)
{
	if (file_write(BIN_DNX, data, sz)) {
		pr_error("Couldn't write dnx file to %s\n", BIN_DNX);
		return -1;
	}

	return 0;
}

static int flash_ifwi(void *data, unsigned sz)
{
	struct firmware_versions img_fw_rev;


	if (access(BIN_DNX, F_OK)) {
		pr_error("dnx binary must be flashed to board first\n");
		return -1;
	}

	if (get_image_fw_rev(data, sz, &img_fw_rev)) {
		pr_error("Coudn't extract FW version data from image");
		return -1;
	}

	printf("Image FW versions:\n");
	dump_fw_versions(&img_fw_rev);

	if (file_write(BIN_IFWI, data, sz)) {
		pr_error("Couldn't write ifwi file to %s\n", BIN_IFWI);
		return -1;
	}

	if (update_ifwi_file(BIN_DNX, BIN_IFWI)) {
		pr_error("IFWI flashing failed!");
		return -1;
	}
	return 0;
}

#define PROXY_SERVICE_NAME		"proxy"
#define PROXY_PROP		"service.proxy.enable"
#define PROXY_START		"1"
#define PROXY_STOP		"0"
#define HSI_PORT	"/sys/bus/hsi/devices/port0"

static int oem_manage_service_proxy(int argc, char **argv)
{
	int retval = 0;

	if ((argc < 2) || (strcmp(argv[0], PROXY_SERVICE_NAME))) {
		/* Should not pass here ! */
		pr_error("oem_manage_service called with wrong parameter!\n");
		retval = -1;
		return retval;
	}

	if (!strcmp(argv[1], "start")) {
		/* Check if HSI node was created, */
		/* indicating that the HSI bus is enabled.*/
		if (-1 != access(HSI_PORT, F_OK))
		{
			/* WORKAROUND */
			/* Check number of cpus => identify CTP (Clovertrail) */
			/* No modem reset for CTP, not supported */
			int fd;
			fd = open("/sys/class/cpuid/cpu3/dev", O_RDONLY);

			if (fd == -1)
			{
				/* Reset the modem */
				pr_info("Reset modem\n");
				reset_modem();
			}
			close(fd);

			/* Start proxy service (at-proxy). */
			property_set(PROXY_PROP, PROXY_START);

		} else {
			pr_error("Fails to find HSI node: %s\n", HSI_PORT);
			retval = -1;
		}

	} else if (!strcmp(argv[1], "stop")) {
		/* Stop proxy service (at-proxy). */
		property_set(PROXY_PROP, PROXY_STOP);

	} else {
		pr_error("Unknown command. Use %s [start/stop].\n", PROXY_SERVICE_NAME);
		retval = -1;
	}

	return retval;
}

#define DNX_TIMEOUT_CHANGE  "dnx_timeout"
#define DNX_TIMEOUT_GET	    "--get"
#define DNX_TIMEOUT_SET	    "--set"
#define SYS_CURRENT_TIMEOUT "/sys/devices/platform/intel_mid_umip/current_timeout"
#define TIMEOUT_SIZE	    20
#define OPTION_SIZE	    6

static int oem_dnx_timeout(int argc, char **argv)
{
	int retval = -1;
	int count, offset, bytes, size;
	int fd;
	char option[OPTION_SIZE] = "";
	char timeout[TIMEOUT_SIZE] = "";
	char check[TIMEOUT_SIZE] = "";

	if (argc < 1 || argc > 3) {
		/* Should not pass here ! */
		fastboot_fail("oem dnx_timeout requires one or two arguments");
		goto end2;
	}

	size = snprintf(option, OPTION_SIZE, "%s", argv[1]);

	if (size == -1 || size > OPTION_SIZE-1) {
	    fastboot_fail("Parameter size exceeds limit");
	    goto end2;
	}

	fd = open(SYS_CURRENT_TIMEOUT, O_RDWR);

	if (fd == -1) {
		pr_error("Can't open %s\n", SYS_CURRENT_TIMEOUT);
		goto end2;
	}

	if (!strcmp(option, DNX_TIMEOUT_GET)) {
		/* Get current timeout */
		count = read(fd, check, TIMEOUT_SIZE);

		if (count <= 0) {
			fastboot_fail("Failed to read");
			goto end1;
		}

		fastboot_info(check);

	} else { if (!strcmp(option, DNX_TIMEOUT_SET)) {
		    /* Set new timeout */

		    if (argc != 3) {
			/* Should not pass here ! */
			fastboot_fail("oem dnx_timeout --set not enough arguments");
			goto end1;
		    }

		    // Get timeout value to set
		    size = snprintf(timeout, TIMEOUT_SIZE, "%s", argv[2]);

		    if (size == -1 || size > TIMEOUT_SIZE-1) {
			fastboot_fail("Timeout value size exceeds limit");
			goto end1;
		    }

		    bytes = write(fd, timeout, size);
		    if (bytes != size) {
			fastboot_fail("oem dnx_timeout failed to write file");
			goto end1;
		    }

		    offset = lseek(fd, 0, SEEK_SET);
		    if (offset == -1) {
			fastboot_fail("oem dnx_timeout failed to set offset");
			goto end1;
		    }

		    memset(check, 0, TIMEOUT_SIZE);

		    count = read(fd, check, TIMEOUT_SIZE);
		    if (count <= 0) {
			fastboot_fail("Failed to check");
		    	goto end1;
		    }

		    // terminate string unconditionally to avoid buffer overflow
		    check[TIMEOUT_SIZE-1] = '\0';
		    if (check[strlen(check)-1] == '\n')
		        check[strlen(check)-1]= '\0';
		    if (strcmp(check, timeout)) {
			fastboot_fail("oem dnx_timeout called with wrong parameter");
			goto end1;
		    }
		} else {
		    fastboot_fail("Unknown command. Use fastboot oem dnx_timeout [--get/--set] command\n");
		    goto end1;
		}
	}

	retval = 0;
	fastboot_okay("");

end1:
	close(fd);
end2:
	return retval;
}

static int oem_nvm_cmd_handler(int argc, char **argv)
{
	int retval = 0;
	char *nvm_path = NULL;

	if (!strcmp(argv[1], "apply")) {
		pr_info("in apply");
		if (argc < 3) {
			pr_error("oem_nvm_cmd_handler called with wrong parameter!\n");
			retval = -1;
			return retval;
		}
		nvm_path = argv[2];

		retval = flash_modem_nvm(nvm_path, nvm_output_callback);
	}
	else if (!strcmp(argv[1], "applyzip")) {
		pr_info("in applyzip");
		if (argc < 3) {
			pr_error("oem_nvm_cmd_handler called with wrong parameter!\n");
			retval = -1;
			return retval;
		}
		nvm_path = argv[2];

		retval = flash_modem_nvm_spid(nvm_path, nvm_output_callback);
	}
        else if (!strcmp(argv[1], "identify")) {
		pr_info("in identify");
		retval = read_modem_nvm_id(NULL, 0, nvm_output_callback);
	}
	else {
		pr_error("Unknown command. Use %s [apply].\n", "nvm");
		retval = -1;
	}

	return retval;
}

#define ERASE_PARTITION     "erase"
#define MOUNT_POINT_SIZE    50      /* /dev/<whatever> */
#define BUFFER_SIZE         4000000 /* 4Mb */

static int oem_erase_partition(int argc, char **argv)
{
	int retval = -1;
	int size;
	char mnt_point[MOUNT_POINT_SIZE] = "";

	if ((argc != 2) || (strcmp(argv[0], ERASE_PARTITION))) {
		/* Should not pass here ! */
                fastboot_fail("oem erase called with wrong parameter!\n");
		goto end;
	}

	if (argv[1][0] == '/') {
		size = snprintf(mnt_point, MOUNT_POINT_SIZE, "%s", argv[1]);

		if (size == -1 || size > MOUNT_POINT_SIZE-1) {
		    fastboot_fail("Mount point parameter size exceeds limit");
		    goto end;
		}
	} else {
		if (!strcmp(argv[1], "userdata")) {
		    strcpy(mnt_point, "/data");
		} else {
		    size = snprintf(mnt_point, MOUNT_POINT_SIZE, "/%s", argv[1]);

		    if (size == -1 || size > MOUNT_POINT_SIZE-1) {
			fastboot_fail("Mount point size exceeds limit");
			goto end;
		    }
		}
	}

	pr_info("CMD '%s %s'...\n", ERASE_PARTITION, mnt_point);

	ui_print("ERASE step 1/2...\n");
	retval = nuke_volume(mnt_point, BUFFER_SIZE);
	if (retval != 0) {
		pr_error("format_volume failed: %s\n", mnt_point);
		goto end;
	} else {
		pr_info("format_volume succeeds: %s\n", mnt_point);
	}

	ui_print("ERASE step 2/2...\n");
	retval = format_volume(mnt_point);
	if (retval != 0) {
		pr_error("format_volume failed: %s\n", mnt_point);
	} else {
		pr_info("format_volume succeeds: %s\n", mnt_point);
	}

end:
    return retval;
}

#define REPART_PARTITION	"repart"

static int oem_repart_partition(int argc, char **argv)
{
	int retval = -1;

	if (argc != 1) {
		/* Should not pass here ! */
		fastboot_fail("oem repart does not require argument");
		goto end;
	}

	retval = ufdisk_create_partition();
	if (retval != 0)
		fastboot_fail("cannot write partition");
	else
		fastboot_okay("");

end:
	return retval;
}

#ifdef USE_GUI
#define PROP_FILE					"/default.prop"
#define SERIAL_NUM_FILE			"/sys/class/android_usb/android0/iSerial"
#define PRODUCT_NAME_ATTR		"ro.product.name"
#define MAX_NAME_SIZE			128
#define BUF_SIZE					256

static char* strupr(char *str)
{
	char *p = str;
	while (*p != '\0') {
		*p = toupper(*p);
		p++;
	}
	return str;
}

static int read_from_file(char* file, char *attr, char *value)
{
	char *p;
	char buf[BUF_SIZE];
	FILE *f;

	if ((f = fopen(file, "r")) == NULL) {
		LOGE("open %s error!\n", file);
		return -1;
	}
	while(fgets(buf, BUF_SIZE, f)) {
		if ((p = strstr(buf, attr)) != NULL) {
			p += strlen(attr)+1;
			strncpy(value, p, MAX_NAME_SIZE);
			value[MAX_NAME_SIZE-1] = '\0';
			strupr(value);
			break;
		}
	}

	fclose(f);
	return 0;
}

static int get_system_info(int type, char *info, unsigned sz)
{
	int ret = -1;
	char pro_name[MAX_NAME_SIZE];
	FILE *f;
	struct firmware_versions v;

	switch (type) {
		case IFWI_VERSION:
			if ((ret = get_current_fw_rev(&v)) < 0)
				break;
			snprintf(info, sz, "%2x.%2x", v.ifwi.major, v.ifwi.minor);
			ret = 0;
			break;
		case PRODUCT_NAME:
			if ((ret = read_from_file(PROP_FILE, PRODUCT_NAME_ATTR, pro_name)) < 0)
				break;
			snprintf(info, sz, "%s", pro_name);
			ret = 0;
			break;
		case SERIAL_NUM:
			if ((f = fopen(SERIAL_NUM_FILE, "r")) == NULL)
				break;
			if (fgets(info, sz, f) == NULL) {
				fclose(f);
				break;
			}
			fclose(f);
			ret = 0;
			break;
		default:
			break;
	}

	return ret;
}
#endif

static void cmd_intel_reboot(const char *arg, void *data, unsigned sz)
{
	fastboot_okay("");
	// This will cause a property trigger in init.rc to cold boot
	property_set("sys.forcecoldboot", "yes");
	sync();
	ui_print("REBOOT...\n");
	pr_info("Rebooting!\n");
	android_reboot(ANDROID_RB_RESTART2, 0, "android");
	pr_error("Reboot failed");
}

static void cmd_intel_reboot_bootloader(const char *arg, void *data, unsigned sz)
{
	fastboot_okay("");
	// No cold boot as it would not allow to reboot in bootloader
	sync();
	ui_print("REBOOT in BOOTLOADER...\n");
	pr_info("Rebooting in BOOTLOADER !\n");
	android_reboot(ANDROID_RB_RESTART2, 0, "bootloader");
	pr_error("Reboot failed");
}

void libintel_droidboot_init(void)
{
	int ret = 0;
	struct OSIP_header osip;
	struct firmware_versions cur_fw_rev;

	ret |= aboot_register_flash_cmd(ANDROID_OS_NAME, flash_android_kernel);
	ret |= aboot_register_flash_cmd(RECOVERY_OS_NAME, flash_recovery_kernel);
	ret |= aboot_register_flash_cmd(FASTBOOT_OS_NAME, flash_fastboot_kernel);
	ret |= aboot_register_flash_cmd(UEFI_FW_NAME, flash_uefi_firmware);
	ret |= aboot_register_flash_cmd("splashscreen", flash_splashscreen_image);
	ret |= aboot_register_flash_cmd("radio", flash_modem);
	ret |= aboot_register_flash_cmd("radio_no_end_reboot", flash_modem_no_end_reboot);
	ret |= aboot_register_flash_cmd("radio_fuse", flash_modem_get_fuse);
	ret |= aboot_register_flash_cmd("radio_erase_all", flash_modem_erase_all);
	ret |= aboot_register_flash_cmd("radio_fuse_only", flash_modem_get_fuse_only);
	ret |= aboot_register_flash_cmd("dnx", flash_dnx);
	ret |= aboot_register_flash_cmd("ifwi", flash_ifwi);

	ret |= aboot_register_flash_cmd("radio_img", flash_modem_store_fw);
	ret |= aboot_register_flash_cmd("rnd_read", flash_modem_read_rnd);
	ret |= aboot_register_flash_cmd("rnd_write", flash_modem_write_rnd);
	ret |= aboot_register_flash_cmd("rnd_erase", flash_modem_erase_rnd);
	ret |= aboot_register_flash_cmd("radio_hwid", flash_modem_get_hw_id);

	ret |= aboot_register_oem_cmd(PROXY_SERVICE_NAME, oem_manage_service_proxy);
	ret |= aboot_register_oem_cmd(DNX_TIMEOUT_CHANGE, oem_dnx_timeout);
	ret |= aboot_register_oem_cmd(ERASE_PARTITION, oem_erase_partition);
	ret |= aboot_register_oem_cmd(REPART_PARTITION, oem_repart_partition);

	ret |= aboot_register_oem_cmd("nvm", oem_nvm_cmd_handler);

	fastboot_register("continue", cmd_intel_reboot);
	fastboot_register("reboot", cmd_intel_reboot);
	fastboot_register("reboot-bootloader", cmd_intel_reboot_bootloader);

#ifdef USE_GUI
	ret |= aboot_register_ui_cmd(UI_GET_SYSTEM_INFO, get_system_info);
#endif

	if (ret)
		die();

	/* Dump the OSIP to serial to assist debugging */
	if (read_OSIP(&osip)) {
		printf("OSIP read failure!\n");
	} else {
		dump_osip_header(&osip);
	}

	if (get_current_fw_rev(&cur_fw_rev)) {
		pr_error("Can't query kernel for current FW version");
	} else {
		printf("Current FW versions: (CHAABI versions unreadable at runtime)\n");
		dump_fw_versions(&cur_fw_rev);
	}
}
