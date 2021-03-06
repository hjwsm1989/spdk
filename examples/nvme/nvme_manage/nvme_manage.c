/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <ctype.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <rte_config.h>
#include <rte_lcore.h>

#include "spdk/nvme.h"
#include "spdk/env.h"

#define MAX_DEVS 64

struct dev {
	struct spdk_pci_addr			pci_addr;
	struct spdk_nvme_ctrlr 			*ctrlr;
	const struct spdk_nvme_ctrlr_data	*cdata;
	struct spdk_nvme_ns_data		*common_ns_data;
	int					outstanding_admin_cmds;
};

static struct dev devs[MAX_DEVS];
static int num_devs = 0;

#define foreach_dev(iter) \
	for (iter = devs; iter - devs < num_devs; iter++)

enum controller_display_model {
	CONTROLLER_DISPLAY_ALL			= 0x0,
	CONTROLLER_DISPLAY_SIMPLISTIC		= 0x1,
};

static int
cmp_devs(const void *ap, const void *bp)
{
	const struct dev *a = ap, *b = bp;

	return spdk_pci_addr_compare(&a->pci_addr, &b->pci_addr);
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_probe_info *probe_info,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	return true;
}

static void
identify_common_ns_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct dev *dev = cb_arg;

	if (cpl->status.sc != SPDK_NVME_SC_SUCCESS) {
		/* Identify Namespace for NSID = FFFFFFFFh is optional, so failure is not fatal. */
		spdk_free(dev->common_ns_data);
		dev->common_ns_data = NULL;
	}

	dev->outstanding_admin_cmds--;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_probe_info *probe_info,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct dev *dev;
	struct spdk_nvme_cmd cmd;

	/* add to dev list */
	dev = &devs[num_devs++];
	dev->pci_addr = probe_info->pci_addr;
	dev->ctrlr = ctrlr;

	/* Retrieve controller data */
	dev->cdata = spdk_nvme_ctrlr_get_data(dev->ctrlr);

	dev->common_ns_data = spdk_zmalloc(sizeof(struct spdk_nvme_ns_data), 4096, NULL);
	if (dev->common_ns_data == NULL) {
		fprintf(stderr, "common_ns_data allocation failure\n");
		return;
	}

	/* Identify Namespace with NSID set to FFFFFFFFh to get common namespace capabilities. */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_IDENTIFY;
	cmd.cdw10 = 0; /* CNS = 0 (Identify Namespace) */
	cmd.nsid = SPDK_NVME_GLOBAL_NS_TAG;

	dev->outstanding_admin_cmds++;
	if (spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, dev->common_ns_data,
					  sizeof(struct spdk_nvme_ns_data), identify_common_ns_cb, dev) != 0) {
		dev->outstanding_admin_cmds--;
		spdk_free(dev->common_ns_data);
		dev->common_ns_data = NULL;
	}

	while (dev->outstanding_admin_cmds) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}
}

static const char *ealargs[] = {
	"nvme_manage",
	"-c 0x1",
	"-n 4",
	"--proc-type=auto",
};

static void usage(void)
{
	printf("NVMe Management Options");
	printf("\n");
	printf("\t[1: list controllers]\n");
	printf("\t[2: create namespace]\n");
	printf("\t[3: delete namespace]\n");
	printf("\t[4: attach namespace to controller]\n");
	printf("\t[5: detach namespace from controller]\n");
	printf("\t[6: format namespace or controller]\n");
	printf("\t[7: firmware update]\n");
	printf("\t[8: quit]\n");
}

static void
display_namespace_dpc(const struct spdk_nvme_ns_data *nsdata)
{
	if (nsdata->dpc.pit1 || nsdata->dpc.pit2 || nsdata->dpc.pit3) {
		if (nsdata->dpc.pit1) {
			printf("PIT1 ");
		}

		if (nsdata->dpc.pit2) {
			printf("PIT2 ");
		}

		if (nsdata->dpc.pit3) {
			printf("PIT3 ");
		}
	} else {
		printf("Not Supported\n");
		return;
	}

	if (nsdata->dpc.md_start && nsdata->dpc.md_end) {
		printf("Location: Head or Tail\n");
	} else if (nsdata->dpc.md_start) {
		printf("Location: Head\n");
	} else if (nsdata->dpc.md_end) {
		printf("Location: Tail\n");
	} else {
		printf("Not Supported\n");
	}
}

static void
display_namespace(struct spdk_nvme_ns *ns)
{
	const struct spdk_nvme_ns_data		*nsdata;
	uint32_t				i;

	nsdata = spdk_nvme_ns_get_data(ns);

	printf("Namespace ID:%d\n", spdk_nvme_ns_get_id(ns));

	printf("Size (in LBAs):              %lld (%lldM)\n",
	       (long long)nsdata->nsze,
	       (long long)nsdata->nsze / 1024 / 1024);
	printf("Capacity (in LBAs):          %lld (%lldM)\n",
	       (long long)nsdata->ncap,
	       (long long)nsdata->ncap / 1024 / 1024);
	printf("Utilization (in LBAs):       %lld (%lldM)\n",
	       (long long)nsdata->nuse,
	       (long long)nsdata->nuse / 1024 / 1024);
	printf("Format Progress Indicator:   %s\n",
	       nsdata->fpi.fpi_supported ? "Supported" : "Not Supported");
	if (nsdata->fpi.fpi_supported && nsdata->fpi.percentage_remaining)
		printf("Formatted Percentage:	%d%%\n", 100 - nsdata->fpi.percentage_remaining);
	printf("Number of LBA Formats:       %d\n", nsdata->nlbaf + 1);
	printf("Current LBA Format:          LBA Format #%02d\n",
	       nsdata->flbas.format);
	for (i = 0; i <= nsdata->nlbaf; i++)
		printf("LBA Format #%02d: Data Size: %5d  Metadata Size: %5d\n",
		       i, 1 << nsdata->lbaf[i].lbads, nsdata->lbaf[i].ms);
	printf("Data Protection Capabilities:");
	display_namespace_dpc(nsdata);
	if (SPDK_NVME_FMT_NVM_PROTECTION_DISABLE == nsdata->dps.pit) {
		printf("Data Protection Setting:     N/A\n");
	} else {
		printf("Data Protection Setting:     PIT%d Location: %s\n",
		       nsdata->dps.pit, nsdata->dps.md_start ? "Head" : "Tail");
	}
	printf("Multipath IO and Sharing:    %s\n",
	       nsdata->nmic.can_share ? "Supported" : "Not Supported");
	printf("\n");
}

static void
display_controller(struct dev *dev, int model)
{
	const struct spdk_nvme_ctrlr_data	*cdata;
	uint8_t					str[128];
	uint32_t				i;

	cdata = spdk_nvme_ctrlr_get_data(dev->ctrlr);

	if (model == CONTROLLER_DISPLAY_SIMPLISTIC) {
		printf("%04x:%02x:%02x.%02x ",
		       dev->pci_addr.domain, dev->pci_addr.bus, dev->pci_addr.dev, dev->pci_addr.func);
		printf("%-40.40s %-20.20s ",
		       cdata->mn, cdata->sn);
		printf("%5d ", cdata->cntlid);
		printf("\n");
		return;
	}

	printf("=====================================================\n");
	printf("NVMe Controller:	%04x:%02x:%02x.%02x\n",
	       dev->pci_addr.domain, dev->pci_addr.bus, dev->pci_addr.dev, dev->pci_addr.func);
	printf("============================\n");
	printf("Controller Capabilities/Features\n");
	printf("Controller ID:		%d\n", cdata->cntlid);
	snprintf(str, sizeof(cdata->sn) + 1, "%s", cdata->sn);
	printf("Serial Number:		%s\n", str);
	printf("\n");

	printf("Admin Command Set Attributes\n");
	printf("============================\n");
	printf("Namespace Manage And Attach:		%s\n",
	       cdata->oacs.ns_manage ? "Supported" : "Not Supported");
	printf("Namespace Format:			%s\n",
	       cdata->oacs.format ? "Supported" : "Not Supported");
	printf("\n");
	printf("NVM Command Set Attributes\n");
	printf("============================\n");
	if (cdata->fna.format_all_ns) {
		printf("Namespace format operation applies to all namespaces\n");
	} else {
		printf("Namespace format operation applies to per namespace\n");
	}
	printf("\n");
	printf("Namespace Attributes\n");
	printf("============================\n");
	for (i = 1; i <= spdk_nvme_ctrlr_get_num_ns(dev->ctrlr); i++) {
		display_namespace(spdk_nvme_ctrlr_get_ns(dev->ctrlr, i));
	}
}

static void
display_controller_list(void)
{
	struct dev			*iter;

	foreach_dev(iter) {
		display_controller(iter, CONTROLLER_DISPLAY_ALL);
	}
}

static struct dev *
get_controller(void)
{
	struct spdk_pci_addr			pci_addr;
	char					address[64];
	char					*p;
	int					ch;
	struct dev				*iter;

	memset(address, 0, sizeof(address));

	foreach_dev(iter) {
		display_controller(iter, CONTROLLER_DISPLAY_SIMPLISTIC);
	}

	printf("Please Input PCI Address(domain:bus:dev.func): \n");

	while ((ch = getchar()) != '\n' && ch != EOF);
	p = fgets(address, 64, stdin);
	if (p == NULL) {
		return NULL;
	}

	while (isspace(*p)) {
		p++;
	}

	if (spdk_pci_addr_parse(&pci_addr, p) < 0) {
		return NULL;
	}

	foreach_dev(iter) {
		if (spdk_pci_addr_compare(&pci_addr, &iter->pci_addr) == 0) {
			return iter;
		}
	}
	return NULL;
}

static int
get_lba_format(const struct spdk_nvme_ns_data *ns_data)
{
	int lbaf, i;

	printf("\nSupported LBA formats:\n");
	for (i = 0; i <= ns_data->nlbaf; i++) {
		printf("%2d: %d data bytes", i, 1 << ns_data->lbaf[i].lbads);
		if (ns_data->lbaf[i].ms) {
			printf(" + %d metadata bytes", ns_data->lbaf[i].ms);
		}
		printf("\n");
	}

	printf("Please input LBA format index (0 - %d):\n", ns_data->nlbaf);
	if (scanf("%d", &lbaf) != 1 || lbaf > ns_data->nlbaf) {
		return -1;
	}

	return lbaf;
}

static void
identify_allocated_ns_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct dev *dev = cb_arg;

	dev->outstanding_admin_cmds--;
}

static uint32_t
get_allocated_nsid(struct dev *dev)
{
	uint32_t nsid;
	size_t i;
	struct spdk_nvme_ns_list *ns_list;
	struct spdk_nvme_cmd cmd = {0};

	ns_list = spdk_zmalloc(sizeof(*ns_list), 4096, NULL);
	if (ns_list == NULL) {
		printf("Allocation error\n");
		return 0;
	}

	cmd.opc = SPDK_NVME_OPC_IDENTIFY;
	cmd.cdw10 = SPDK_NVME_IDENTIFY_ALLOCATED_NS_LIST;
	cmd.nsid = 0;

	dev->outstanding_admin_cmds++;
	if (spdk_nvme_ctrlr_cmd_admin_raw(dev->ctrlr, &cmd, ns_list, sizeof(*ns_list),
					  identify_allocated_ns_cb, dev)) {
		printf("Identify command failed\n");
		spdk_free(ns_list);
		return 0;
	}

	while (dev->outstanding_admin_cmds) {
		spdk_nvme_ctrlr_process_admin_completions(dev->ctrlr);
	}

	printf("Allocated Namespace IDs:\n");
	for (i = 0; i < sizeof(ns_list->ns_list) / sizeof(*ns_list->ns_list); i++) {
		if (ns_list->ns_list[i] == 0) {
			break;
		}
		printf("%u\n", ns_list->ns_list[i]);
	}

	spdk_free(ns_list);

	printf("Please Input Namespace ID: \n");
	if (!scanf("%u", &nsid)) {
		printf("Invalid Namespace ID\n");
		nsid = 0;
	}

	return nsid;
}

static void
ns_attach(struct dev *device, int attachment_op, int ctrlr_id, int ns_id)
{
	int ret = 0;
	struct spdk_nvme_ctrlr_list *ctrlr_list;

	ctrlr_list = spdk_zmalloc(sizeof(struct spdk_nvme_ctrlr_list),
				  4096, NULL);
	if (ctrlr_list == NULL) {
		printf("Allocation error (controller list)\n");
		exit(1);
	}

	ctrlr_list->ctrlr_count = 1;
	ctrlr_list->ctrlr_list[0] = ctrlr_id;

	if (attachment_op == SPDK_NVME_NS_CTRLR_ATTACH) {
		ret = spdk_nvme_ctrlr_attach_ns(device->ctrlr, ns_id, ctrlr_list);
	} else if (attachment_op == SPDK_NVME_NS_CTRLR_DETACH) {
		ret = spdk_nvme_ctrlr_detach_ns(device->ctrlr, ns_id, ctrlr_list);
	}

	if (ret) {
		fprintf(stdout, "ns attach: Failed\n");
	}

	spdk_free(ctrlr_list);
}

static void
ns_manage_add(struct dev *device, uint64_t ns_size, uint64_t ns_capacity, int ns_lbasize,
	      uint8_t ns_dps_type, uint8_t ns_dps_location, uint8_t ns_nmic)
{
	uint32_t nsid;
	struct spdk_nvme_ns_data *ndata;

	ndata = spdk_zmalloc(sizeof(struct spdk_nvme_ns_data), 4096, NULL);
	if (ndata == NULL) {
		printf("Allocation error (namespace data)\n");
		exit(1);
	}

	ndata->nsze = ns_size;
	ndata->ncap = ns_capacity;
	ndata->flbas.format = ns_lbasize;
	if (SPDK_NVME_FMT_NVM_PROTECTION_DISABLE != ns_dps_type) {
		ndata->dps.pit = ns_dps_type;
		ndata->dps.md_start = ns_dps_location;
	}
	ndata->nmic.can_share = ns_nmic;
	nsid = spdk_nvme_ctrlr_create_ns(device->ctrlr, ndata);
	if (nsid == 0) {
		fprintf(stdout, "ns manage: Failed\n");
	} else {
		printf("Created namespace ID %u\n", nsid);
	}

	spdk_free(ndata);
}

static void
ns_manage_delete(struct dev *device, int ns_id)
{
	int ret = 0;

	ret = spdk_nvme_ctrlr_delete_ns(device->ctrlr, ns_id);
	if (ret) {
		fprintf(stdout, "ns manage: Failed\n");
		return;
	}
}

static void
nvme_manage_format(struct dev *device, int ns_id, int ses, int pi, int pil, int ms, int lbaf)
{
	int ret = 0;
	struct spdk_nvme_format format = {};

	format.lbaf	= lbaf;
	format.ms	= ms;
	format.pi	= pi;
	format.pil	= pil;
	format.ses	= ses;
	ret = spdk_nvme_ctrlr_format(device->ctrlr, ns_id, &format);
	if (ret) {
		fprintf(stdout, "nvme format: Failed\n");
		return;
	}
}

static void
attach_and_detach_ns(int attachment_op)
{
	uint32_t	nsid;
	struct dev	*ctrlr;

	ctrlr = get_controller();
	if (ctrlr == NULL) {
		printf("Invalid controller PCI Address.\n");
		return;
	}

	if (!ctrlr->cdata->oacs.ns_manage) {
		printf("Controller does not support ns management\n");
		return;
	}

	nsid = get_allocated_nsid(ctrlr);
	if (nsid == 0) {
		printf("Invalid Namespace ID\n");
		return;
	}

	ns_attach(ctrlr, attachment_op, ctrlr->cdata->cntlid, nsid);
}

static void
add_ns(void)
{
	uint64_t	ns_size		= 0;
	uint64_t	ns_capacity	= 0;
	int		ns_lbasize;
	int 		ns_dps_type  	= 0;
	int 		ns_dps_location = 0;
	int	 	ns_nmic 	= 0;
	struct dev	*ctrlr		= NULL;

	ctrlr = get_controller();
	if (ctrlr == NULL) {
		printf("Invalid controller PCI Address.\n");
		return;
	}

	if (!ctrlr->cdata->oacs.ns_manage) {
		printf("Controller does not support ns management\n");
		return;
	}

	if (!ctrlr->common_ns_data) {
		printf("Controller did not return common namespace capabilities\n");
		return;
	}

	ns_lbasize = get_lba_format(ctrlr->common_ns_data);
	if (ns_lbasize < 0) {
		printf("Invalid LBA format number\n");
		return;
	}

	printf("Please Input Namespace Size (in LBAs): \n");
	if (!scanf("%" SCNi64, &ns_size)) {
		printf("Invalid Namespace Size\n");
		while (getchar() != '\n');
		return;
	}

	printf("Please Input Namespace Capacity (in LBAs): \n");
	if (!scanf("%" SCNi64, &ns_capacity)) {
		printf("Invalid Namespace Capacity\n");
		while (getchar() != '\n');
		return;
	}

	printf("Please Input Data Protection Type (0 - 3): \n");
	if (!scanf("%d", &ns_dps_type)) {
		printf("Invalid Data Protection Type\n");
		while (getchar() != '\n');
		return;
	}

	if (SPDK_NVME_FMT_NVM_PROTECTION_DISABLE != ns_dps_type) {
		printf("Please Input Data Protection Location (1: Head; 0: Tail): \n");
		if (!scanf("%d", &ns_dps_location)) {
			printf("Invalid Data Protection Location\n");
			while (getchar() != '\n');
			return;
		}
	}

	printf("Please Input Multi-path IO and Sharing Capabilities (1: Share; 0: Private): \n");
	if (!scanf("%d", &ns_nmic)) {
		printf("Invalid Multi-path IO and Sharing Capabilities\n");
		while (getchar() != '\n');
		return;
	}

	ns_manage_add(ctrlr, ns_size, ns_capacity, ns_lbasize,
		      ns_dps_type, ns_dps_location, ns_nmic);
}

static void
delete_ns(void)
{
	int 					ns_id;
	struct dev				*ctrlr;

	ctrlr = get_controller();
	if (ctrlr == NULL) {
		printf("Invalid controller PCI Address.\n");
		return;
	}

	if (!ctrlr->cdata->oacs.ns_manage) {
		printf("Controller does not support ns management\n");
		return;
	}

	printf("Please Input Namespace ID: \n");
	if (!scanf("%d", &ns_id)) {
		printf("Invalid Namespace ID\n");
		while (getchar() != '\n');
		return;
	}

	ns_manage_delete(ctrlr, ns_id);
}

static void
format_nvm(void)
{
	int 					ns_id;
	int					ses;
	int					pil;
	int					pi;
	int					ms;
	int					lbaf;
	char					option;
	struct dev				*ctrlr;
	const struct spdk_nvme_ctrlr_data	*cdata;
	struct spdk_nvme_ns			*ns;
	const struct spdk_nvme_ns_data		*nsdata;

	ctrlr = get_controller();
	if (ctrlr == NULL) {
		printf("Invalid controller PCI BDF.\n");
		return;
	}

	cdata = ctrlr->cdata;

	if (!cdata->oacs.format) {
		printf("Controller does not support Format NVM command\n");
		return;
	}

	if (cdata->fna.format_all_ns) {
		ns_id = SPDK_NVME_GLOBAL_NS_TAG;
		ns = spdk_nvme_ctrlr_get_ns(ctrlr->ctrlr, 1);
	} else {
		printf("Please Input Namespace ID (1 - %d): \n", cdata->nn);
		if (!scanf("%d", &ns_id)) {
			printf("Invalid Namespace ID\n");
			while (getchar() != '\n');
			return;
		}
		ns = spdk_nvme_ctrlr_get_ns(ctrlr->ctrlr, ns_id);
	}

	if (ns == NULL) {
		printf("Namespace ID %d not found\n", ns_id);
		while (getchar() != '\n');
		return;
	}

	nsdata = spdk_nvme_ns_get_data(ns);

	printf("Please Input Secure Erase Setting: \n");
	printf("	0: No secure erase operation requested\n");
	printf("	1: User data erase\n");
	if (cdata->fna.crypto_erase_supported) {
		printf("	2: Cryptographic erase\n");
	}
	if (!scanf("%d", &ses)) {
		printf("Invalid Secure Erase Setting\n");
		while (getchar() != '\n');
		return;
	}

	lbaf = get_lba_format(nsdata);
	if (lbaf < 0) {
		printf("Invalid LBA format number\n");
		return;
	}

	if (nsdata->lbaf[lbaf].ms) {
		printf("Please Input Protection Information: \n");
		printf("	0: Protection information is not enabled\n");
		printf("	1: Protection information is enabled, Type 1\n");
		printf("	2: Protection information is enabled, Type 2\n");
		printf("	3: Protection information is enabled, Type 3\n");
		if (!scanf("%d", &pi)) {
			printf("Invalid protection information\n");
			while (getchar() != '\n');
			return;
		}

		if (pi) {
			printf("Please Input Protection Information Location: \n");
			printf("	0: Protection information transferred as the last eight bytes of metadata\n");
			printf("	1: Protection information transferred as the first eight bytes of metadata\n");
			if (!scanf("%d", &pil)) {
				printf("Invalid protection information location\n");
				while (getchar() != '\n');
				return;
			}
		} else {
			pil = 0;
		}

		printf("Please Input Metadata Setting: \n");
		printf("	0: Metadata is transferred as part of a separate buffer\n");
		printf("	1: Metadata is transferred as part of an extended data LBA\n");
		if (!scanf("%d", &ms)) {
			printf("Invalid metadata setting\n");
			while (getchar() != '\n');
			return;
		}
	} else {
		ms = 0;
		pi = 0;
		pil = 0;
	}

	printf("Warning: use this utility at your own risk.\n"
	       "This command will format your namespace and all data will be lost.\n"
	       "This command may take several minutes to complete,\n"
	       "so do not interrupt the utility until it completes.\n"
	       "Press 'Y' to continue with the format operation.\n");

	while (getchar() != '\n');
	if (!scanf("%c", &option)) {
		printf("Invalid option\n");
		while (getchar() != '\n');
		return;
	}

	if (option == 'y' || option == 'Y') {
		nvme_manage_format(ctrlr, ns_id, ses, pi, pil, ms, lbaf);
	} else {
		printf("NVMe format abort\n");
	}
}

static void
update_firmware_image(void)
{
	int					rc;
	int					fd = -1;
	int					slot;
	unsigned int				size;
	struct stat				fw_stat;
	char					path[256];
	void					*fw_image;
	struct dev				*ctrlr;
	const struct spdk_nvme_ctrlr_data	*cdata;

	ctrlr = get_controller();
	if (ctrlr == NULL) {
		printf("Invalid controller PCI BDF.\n");
		return;
	}

	cdata = ctrlr->cdata;

	if (!cdata->oacs.firmware) {
		printf("Controller does not support firmware download and commit command\n");
		return;
	}

	printf("Please Input The Path Of Firmware Image\n");

	if (fgets(path, 256, stdin) == NULL) {
		printf("Invalid path setting\n");
		while (getchar() != '\n');
		return;
	}
	path[strlen(path) - 1] = '\0';

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror("Open file failed");
		return;
	}
	rc = fstat(fd, &fw_stat);
	if (rc < 0) {
		printf("Fstat failed\n");
		close(fd);
		return;
	}

	if (fw_stat.st_size % 4) {
		printf("Firmware image size is not multiple of 4\n");
		close(fd);
		return;
	}

	size = fw_stat.st_size;

	fw_image = spdk_zmalloc(size, 4096, NULL);
	if (fw_image == NULL) {
		printf("Allocation error\n");
		close(fd);
		return;
	}

	if (read(fd, fw_image, size) != ((ssize_t)(size))) {
		printf("Read firmware image failed\n");
		close(fd);
		spdk_free(fw_image);
		return;
	}
	close(fd);

	printf("Please Input Slot(0 - 7): \n");
	if (!scanf("%d", &slot)) {
		printf("Invalid Slot\n");
		spdk_free(fw_image);
		while (getchar() != '\n');
		return;
	}

	rc = spdk_nvme_ctrlr_update_firmware(ctrlr->ctrlr, fw_image, size, slot);
	if (rc) {
		printf("spdk_nvme_ctrlr_update_firmware failed\n");
	} else {
		printf("spdk_nvme_ctrlr_update_firmware success\n");
	}
	spdk_free(fw_image);
}

int main(int argc, char **argv)
{
	int				rc, i;

	rc = rte_eal_init(sizeof(ealargs) / sizeof(ealargs[0]),
			  (char **)(void *)(uintptr_t)ealargs);

	if (rc < 0) {
		fprintf(stderr, "could not initialize dpdk\n");
		exit(1);
	}

	if (spdk_nvme_probe(NULL, probe_cb, attach_cb, NULL) != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		return 1;
	}

	qsort(devs, num_devs, sizeof(devs[0]), cmp_devs);

	usage();

	while (1) {
		int cmd;
		bool exit_flag = false;

		if (!scanf("%d", &cmd)) {
			printf("Invalid Command\n");
			while (getchar() != '\n');
			return 0;
		}
		switch (cmd) {
		case 1:
			display_controller_list();
			break;
		case 2:
			add_ns();
			break;
		case 3:
			delete_ns();
			break;
		case 4:
			attach_and_detach_ns(SPDK_NVME_NS_CTRLR_ATTACH);
			break;
		case 5:
			attach_and_detach_ns(SPDK_NVME_NS_CTRLR_DETACH);
			break;
		case 6:
			format_nvm();
			break;
		case 7:
			update_firmware_image();
			break;
		case 8:
			exit_flag = true;
			break;
		default:
			printf("Invalid Command\n");
			break;
		}

		if (exit_flag)
			break;

		while (getchar() != '\n');
		printf("press Enter to display cmd menu ...\n");
		while (getchar() != '\n');
		usage();
	}

	printf("Cleaning up...\n");

	for (i = 0; i < num_devs; i++) {
		struct dev *dev = &devs[i];
		spdk_nvme_detach(dev->ctrlr);
	}

	return rc;
}
