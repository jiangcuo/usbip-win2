#include "vhci_urbr_fetch_bulk.h"
#include "vhci_driver.h"

#include "usbip_proto.h"
#include "vhci_urbr.h"
#include "vhci_urbr_fetch.h"

NTSTATUS
fetch_urbr_bulk_or_interrupt(PURB urb, struct usbip_header *hdr)
{
	struct _URB_BULK_OR_INTERRUPT_TRANSFER	*urb_bi = &urb->UrbBulkOrInterruptTransfer;

	if (IS_TRANSFER_FLAGS_IN(urb_bi->TransferFlags)) {
		NTSTATUS	status;
		status = copy_to_transfer_buffer(urb_bi->TransferBuffer, urb_bi->TransferBufferMDL,
			urb_bi->TransferBufferLength, hdr + 1, hdr->u.ret_submit.actual_length);
		if (status == STATUS_SUCCESS)
			urb_bi->TransferBufferLength = hdr->u.ret_submit.actual_length;
		return status;
	}
	return STATUS_SUCCESS;
}
