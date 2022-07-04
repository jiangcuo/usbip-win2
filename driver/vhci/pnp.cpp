#include "pnp.h"
#include "trace.h"
#include "pnp.tmh"

#include "vhci.h"
#include "pnp_id.h"
#include "irp.h"
#include "pnp_intf.h"
#include "pnp_relations.h"
#include "pnp_cap.h"
#include "pnp_start.h"
#include "pnp_remove.h"
#include "pnp_resources.h"
#include "vhub.h"
#include "strutil.h"

#include <wdmguid.h>

namespace
{

const LPCWSTR vdev_desc[] = 
{
	L"usbip-win ROOT", 
	L"usbip-win CPDO", 
	L"usbip-win VHCI", 
	L"usbip-win HPDO", 
	L"usbip-win VHUB", 
	L"usbip-win VPDO"
};
static_assert(ARRAYSIZE(vdev_desc) == VDEV_SIZE);

_IRQL_requires_(PASSIVE_LEVEL)
PAGEABLE auto irp_pass_down_or_complete(vdev_t *vdev, IRP *irp)
{
	return is_fdo(vdev->type) ? irp_pass_down(vdev->devobj_lower, irp) : CompleteRequest(irp);
}

_IRQL_requires_(PASSIVE_LEVEL)
PAGEABLE NTSTATUS pnp_query_stop_device(vdev_t *vdev, IRP *irp)
{
	PAGED_CODE();
	TraceMsg("%!vdev_type_t!(%04x)", vdev->type, ptr4log(vdev));

	set_state(*vdev, pnp_state::StopPending);
	return irp_pass_down_or_complete(vdev, irp);
}

_IRQL_requires_(PASSIVE_LEVEL)
PAGEABLE NTSTATUS pnp_cancel_stop_device(vdev_t *vdev, IRP *irp)
{
	PAGED_CODE();
	TraceMsg("%!vdev_type_t!(%04x)", vdev->type, ptr4log(vdev));

	if (vdev->PnPState == pnp_state::StopPending) {
		set_previous_pnp_state(*vdev);
	}

	return irp_pass_down_or_complete(vdev, irp);
}

_IRQL_requires_(PASSIVE_LEVEL)
PAGEABLE NTSTATUS pnp_stop_device(vdev_t *vdev, IRP *irp)
{
	PAGED_CODE();
	TraceMsg("%!vdev_type_t!(%04x)", vdev->type, ptr4log(vdev));

	set_state(*vdev, pnp_state::Stopped);
	return irp_pass_down_or_complete(vdev, irp);
}

_IRQL_requires_(PASSIVE_LEVEL)
PAGEABLE auto device_can_be_removed(_In_ vdev_t *vdev)
{
	PAGED_CODE();
	LARGE_INTEGER timeout{};
	return !KeWaitForSingleObject(&vdev->intf_ref_event, Executive, KernelMode, false, &timeout);
}

_IRQL_requires_(PASSIVE_LEVEL)
PAGEABLE NTSTATUS pnp_query_remove_device(vdev_t *vdev, IRP *irp)
{
	PAGED_CODE();
	TraceMsg("%!vdev_type_t!(%04x)", vdev->type, ptr4log(vdev));

	if (device_can_be_removed(vdev)) {
		set_state(*vdev, pnp_state::RemovePending);
		return irp_pass_down_or_complete(vdev, irp);
	} else {
		TraceMsg("Can't be removed, intf_ref_cnt %ld", vdev->intf_ref_cnt);
		return CompleteRequest(irp, STATUS_UNSUCCESSFUL);
	}
}

_IRQL_requires_(PASSIVE_LEVEL)
PAGEABLE NTSTATUS pnp_cancel_remove_device(vdev_t *vdev, IRP *irp)
{
	PAGED_CODE();
	TraceMsg("%!vdev_type_t!(%04x)", vdev->type, ptr4log(vdev));

	if (vdev->PnPState == pnp_state::RemovePending) {
		set_previous_pnp_state(*vdev);
	}

	return irp_pass_down_or_complete(vdev, irp);
}

_IRQL_requires_(PASSIVE_LEVEL)
PAGEABLE NTSTATUS pnp_surprise_removal(vdev_t *vdev, IRP *irp)
{
	PAGED_CODE();
	TraceMsg("%!vdev_type_t!(%04x)", vdev->type, ptr4log(vdev));

	set_state(*vdev, pnp_state::SurpriseRemovePending);
	return irp_pass_down_or_complete(vdev, irp);
}

_IRQL_requires_(PASSIVE_LEVEL)
PAGEABLE NTSTATUS pnp_query_bus_information(vdev_t *vdev, IRP *irp)
{
	PAGED_CODE();
	TraceMsg("%!vdev_type_t!(%04x)", vdev->type, ptr4log(vdev));

	PNP_BUS_INFORMATION *bi = (PNP_BUS_INFORMATION*)ExAllocatePool2(POOL_FLAG_PAGED|POOL_FLAG_UNINITIALIZED, sizeof(*bi), USBIP_VHCI_POOL_TAG);
	if (bi) {
		bi->BusTypeGuid = GUID_BUS_TYPE_USB;
		bi->LegacyBusType = PNPBus;
		bi->BusNumber = 1; // arbitrary
	}

	irp->IoStatus.Information = reinterpret_cast<ULONG_PTR>(bi);

	auto st = bi ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
	return CompleteRequest(irp, st);
}

_IRQL_requires_(PASSIVE_LEVEL)
PAGEABLE NTSTATUS pnp_0x0E(vdev_t *vdev, IRP *irp)
{
	PAGED_CODE();
	TraceMsg("%!vdev_type_t!(%04x)", vdev->type, ptr4log(vdev));
	return CompleteRequestAsIs(irp);
}

_IRQL_requires_max_(APC_LEVEL)
PAGEABLE NTSTATUS pnp_read_config(vdev_t *vdev, IRP *irp)
{
	PAGED_CODE();

	NT_ASSERT(irp->IoStatus.Status == STATUS_NOT_SUPPORTED);
	NT_ASSERT(!irp->IoStatus.Information);

	TraceMsg("%!vdev_type_t!(%04x)", vdev->type, ptr4log(vdev));
	return CompleteRequestAsIs(irp);
}

_IRQL_requires_max_(APC_LEVEL)
PAGEABLE NTSTATUS pnp_write_config(vdev_t *vdev, IRP *irp)
{
	PAGED_CODE();

	NT_ASSERT(irp->IoStatus.Status == STATUS_NOT_SUPPORTED);
	NT_ASSERT(!irp->IoStatus.Information);

	TraceMsg("%!vdev_type_t!(%04x)", vdev->type, ptr4log(vdev));
	return CompleteRequestAsIs(irp);
}

/*
* For the device to be ejected, the device must be in the D3
* device power state (off) and must be unlocked
* (if the device supports locking). Any driver that returns success
* for this IRP must wait until the device has been ejected before
* completing the IRP.
*/
_IRQL_requires_(PASSIVE_LEVEL)
PAGEABLE NTSTATUS pnp_eject(vdev_t *vdev, IRP *irp)
{
	PAGED_CODE();
	TraceMsg("%!vdev_type_t!(%04x)", vdev->type, ptr4log(vdev));

	if (vdev->type == VDEV_VPDO) {
		vhub_unplug_vpdo(static_cast<vpdo_dev_t*>(vdev));
		return CompleteRequest(irp);
	}

	return CompleteRequestAsIs(irp);
}

_IRQL_requires_(PASSIVE_LEVEL)
PAGEABLE NTSTATUS pnp_set_lock(vdev_t *vdev, IRP *irp)
{
	PAGED_CODE();
	TraceMsg("%!vdev_type_t!(%04x)", vdev->type, ptr4log(vdev));
	return CompleteRequestAsIs(irp);
}

_IRQL_requires_(PASSIVE_LEVEL)
PAGEABLE NTSTATUS pnp_query_pnp_device_state(vdev_t *vdev, IRP *irp)
{
	PAGED_CODE();

	auto &st = reinterpret_cast<PNP_DEVICE_STATE&>(irp->IoStatus.Information);

	if (vdev->PnPState == pnp_state::Removed) {
		st |= PNP_DEVICE_REMOVED;
	}

	TraceMsg("%!vdev_type_t!(%04x): %#lx", vdev->type, ptr4log(vdev), st);
	return CompleteRequest(irp);
}

_IRQL_requires_(PASSIVE_LEVEL)
PAGEABLE NTSTATUS pnp_device_usage_notification(vdev_t *vdev, IRP *irp)
{
	PAGED_CODE();
	TraceMsg("%!vdev_type_t!(%04x)", vdev->type, ptr4log(vdev));

	if (auto r = reinterpret_cast<IO_RESOURCE_REQUIREMENTS_LIST*>(irp->IoStatus.Information)) {
		TraceDbg("ListSize %lu, InterfaceType %d, BusNumber %lu, SlotNumber %lu, AlternativeLists %lu", 
			  r->ListSize, r->InterfaceType, r->BusNumber, r->SlotNumber, r->AlternativeLists);
	}

	return irp_pass_down_or_complete(vdev, irp);
}

_IRQL_requires_(PASSIVE_LEVEL)
PAGEABLE NTSTATUS pnp_query_legacy_bus_information(vdev_t *vdev, IRP *irp)
{
	PAGED_CODE();
	TraceMsg("%!vdev_type_t!(%04x)", vdev->type, ptr4log(vdev));
	return CompleteRequestAsIs(irp);
}

/*
 * This request notifies bus drivers that a device object exists and
 * that it has been fully enumerated by the plug and play manager.
 */
_IRQL_requires_(PASSIVE_LEVEL)
PAGEABLE NTSTATUS pnp_device_enumerated(vdev_t *vdev, IRP *irp)
{
	PAGED_CODE();
	TraceMsg("%!vdev_type_t!(%04x)", vdev->type, ptr4log(vdev));
	return CompleteRequest(irp);
}

_IRQL_requires_(PASSIVE_LEVEL)
PAGEABLE void copy_str(LPCWSTR s, IO_STATUS_BLOCK &blk)
{
	PAGED_CODE();
	if (auto str = libdrv_strdup(POOL_FLAG_PAGED, s)) {
		blk.Information = reinterpret_cast<ULONG_PTR>(str);
		blk.Status = STATUS_SUCCESS;
	}
}

/*
 * Bus drivers must handle this request for their child devices if the bus supports this information. 
 * Function and filter drivers do not handle this IRP.
 * If a bus driver returns information in response to this IRP, 
 * it allocates a NULL-terminated Unicode string from paged memory. 
 */
_IRQL_requires_(PASSIVE_LEVEL)
PAGEABLE NTSTATUS pnp_query_device_text(vdev_t *vdev, IRP *irp)
{
	PAGED_CODE();

	auto &Status = irp->IoStatus.Status;

	auto &Information = irp->IoStatus.Information;
	NT_ASSERT(!Information);

	auto irpstack = IoGetCurrentIrpStackLocation(irp);
	auto &r = irpstack->Parameters.QueryDeviceText;

	DEVICE_REGISTRY_PROPERTY prop;
	LPCWSTR prop_str{};

	switch (auto type = r.DeviceTextType) {
	case DeviceTextDescription:
		prop = DevicePropertyDeviceDescription;
		NT_ASSERT(vdev->type < ARRAYSIZE(vdev_desc));
		prop_str = vdev_desc[vdev->type];
		break;
	case DeviceTextLocationInformation:
		prop = DevicePropertyLocationInformation;
		break;
	default:
		Trace(TRACE_LEVEL_ERROR, "%!vdev_type_t!: unknown DeviceTextType %d, LocaleId %#x", vdev->type, type, r.LocaleId);
		return CompleteRequest(irp, STATUS_INVALID_PARAMETER);
	}

	NTSTATUS err;
	ULONG dummy;

	if (auto str = GetDeviceProperty(vdev->Self, prop, err, dummy)) {
		Information = reinterpret_cast<ULONG_PTR>(str);
		Status = STATUS_SUCCESS;
	} else if (vdev->type == VDEV_VPDO && prop == DevicePropertyDeviceDescription) {
		auto vpdo = reinterpret_cast<vpdo_dev_t*>(vdev);
		if (auto prod = get_product(*vpdo)) {
			copy_str(prod, irp->IoStatus);
		}
	}

	if (!Information && prop_str) {
		copy_str(prop_str, irp->IoStatus);
	}
	
	TraceMsg("%!vdev_type_t!: %!DEVICE_TEXT_TYPE!, LCID %#lx -> '%!WSTR!', %!STATUS!", 
		  vdev->type, r.DeviceTextType, r.LocaleId, reinterpret_cast<wchar_t*>(Information), Status);

	return CompleteRequestAsIs(irp);
}

using pnpmn_func_t = NTSTATUS(vdev_t*, IRP*);

pnpmn_func_t* const pnpmn_functions[] =
{
	pnp_start_device, // IRP_MN_START_DEVICE
	pnp_query_remove_device,
	pnp_remove_device,
	pnp_cancel_remove_device,
	pnp_stop_device,
	pnp_query_stop_device,
	pnp_cancel_stop_device,

	pnp_query_device_relations,
	pnp_query_interface,
	pnp_query_capabilities,
	pnp_query_resources,
	pnp_query_resource_requirements,
	pnp_query_device_text,
	pnp_filter_resource_requirements,

	pnp_0x0E, // 0x0E, undefined

	pnp_read_config,
	pnp_write_config,
	pnp_eject,
	pnp_set_lock,
	pnp_query_id,
	pnp_query_pnp_device_state,
	pnp_query_bus_information,
	pnp_device_usage_notification,
	pnp_surprise_removal,

	pnp_query_legacy_bus_information, // IRP_MN_QUERY_LEGACY_BUS_INFORMATION
	pnp_device_enumerated // IRP_MN_DEVICE_ENUMERATED, since WIN7
};

} // namespace


_IRQL_requires_(PASSIVE_LEVEL)
PAGEABLE void set_state(vdev_t &vdev, pnp_state state)
{
	PAGED_CODE();
	vdev.PreviousPnPState = vdev.PnPState;
	vdev.PnPState = state;
}

_IRQL_requires_(PASSIVE_LEVEL)
_IRQL_requires_same_
extern "C" PAGEABLE NTSTATUS vhci_pnp(__in PDEVICE_OBJECT devobj, __in IRP *irp)
{
	PAGED_CODE();

	auto vdev = to_vdev(devobj);
	auto irpstack = IoGetCurrentIrpStackLocation(irp);

	TraceDbg("%!vdev_type_t!: enter irql %!irql!, %!pnpmn!", vdev->type, KeGetCurrentIrql(), irpstack->MinorFunction);

	auto status = STATUS_SUCCESS;

	if (vdev->PnPState == pnp_state::Removed) { // the driver should not pass the IRP down to the next lower driver
		status = CompleteRequest(irp, STATUS_NO_SUCH_DEVICE);
	} else if (irpstack->MinorFunction < ARRAYSIZE(pnpmn_functions)) {
		status = pnpmn_functions[irpstack->MinorFunction](vdev, irp);
	} else {
		Trace(TRACE_LEVEL_WARNING, "%!vdev_type_t!: unknown MinorFunction %!pnpmn!", vdev->type, irpstack->MinorFunction);
		status = CompleteRequestAsIs(irp);
	}

	TraceDbg("%!vdev_type_t!: leave %!STATUS!", vdev->type, status);
	return status;
}
