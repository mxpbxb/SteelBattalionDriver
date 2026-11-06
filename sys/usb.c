/*

Copyright (c) Oscar Sebio Caajaraville 2019.

*/

#include "hidusbsteelbattalion.h"

#if defined(EVENT_TRACING)
#include "usb.tmh"
#endif

static VOID HidSteelBattalionPrepareLeds(IN WDFDEVICE Device, IN PDEVICE_EXTENSION DeviceContext);

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, HidSteelBattalionPrepareLeds)
#pragma alloc_text(PAGE, HidSteelBattalionEvtDevicePrepareHardware)
#pragma alloc_text(PAGE, HidSteelBattalionEvtDeviceD0Exit)
#pragma alloc_text(PAGE, HidSteelBattalionConfigContReaderForInterruptEndPoint)
#pragma alloc_text(PAGE, HidSteelBattalionValidateConfigurationDescriptor)
#endif

NTSTATUS HidSteelBattalionEvtDevicePrepareHardware
(
	IN WDFDEVICE    Device,
    IN WDFCMRESLIST ResourceList,
    IN WDFCMRESLIST ResourceListTranslated
)
/*++
Routine Description:
    In this callback, the driver does whatever is necessary to make the
    hardware ready to use.  In the case of a USB device, this involves
    reading and selecting descriptors.

Arguments:
    Device - handle to a device

    ResourceList - A handle to a framework resource-list object that
    identifies the raw hardware resourcest

    ResourceListTranslated - A handle to a framework resource-list object
    that identifies the translated hardware resources

Return Value:
    NT status value
--*/
{
    NTSTATUS                            status = STATUS_SUCCESS;
    PDEVICE_EXTENSION                   devContext = NULL;
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS configParams;
    WDF_OBJECT_ATTRIBUTES               attributes;
    PUSB_DEVICE_DESCRIPTOR              usbDeviceDescriptor = NULL;

    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(ResourceListTranslated);

    PAGED_CODE ();

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT, "HidSteelBattalionEvtDevicePrepareHardware Enter\n");

    devContext = GetDeviceContext(Device);

    // Create a WDFUSBDEVICE object. WdfUsbTargetDeviceCreate obtains the
    // USB device descriptor and the first USB configuration descriptor from
    // the device and stores them. It also creates a framework USB interface
    // object for each interface in the device's first configuration.
    //
    // The parent of each USB device object is the driver's framework driver
    // object. The driver cannot change this parent, and the ParentObject
    // member or the WDF_OBJECT_ATTRIBUTES structure must be NULL.
    //
    // We only create device the first time PrepareHardware is called. If
    // the device is restarted by pnp manager for resource rebalance, we
    // will use the same device handle but then select the interfaces again
    // because the USB stack could reconfigure the device on restart.
    if (devContext->UsbDevice == NULL) 
	{
        status = WdfUsbTargetDeviceCreate(Device, WDF_NO_OBJECT_ATTRIBUTES, &devContext->UsbDevice);
        if (!NT_SUCCESS(status)) 
		{
            TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "WdfUsbTargetDeviceCreate failed 0x%x\n", status);
            return status;
        }

        // TODO: If you are fetching configuration descriptor from device for
        // selecting a configuration or to parse other descriptors, call
        // HidSteelBattalionValidateConfigurationDescriptor
        // to do basic validation on the descriptors before you access them.
    }

    // Select a device configuration by using a
    // WDF_USB_DEVICE_SELECT_CONFIG_PARAMS structure to specify USB
    // descriptors, a URB, or handles to framework USB interface objects.
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_SINGLE_INTERFACE( &configParams);

    status = WdfUsbTargetDeviceSelectConfig(devContext->UsbDevice, WDF_NO_OBJECT_ATTRIBUTES, &configParams);
    if(!NT_SUCCESS(status)) 
	{
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "WdfUsbTargetDeviceSelectConfig failed %!STATUS!\n", status);
        return status;
    }

    devContext->UsbInterface = configParams.Types.SingleInterface.ConfiguredUsbInterface;

    // Get the device descriptor and store it in device context
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Device;
    status = WdfMemoryCreate(&attributes, NonPagedPoolNx, 0, sizeof(USB_DEVICE_DESCRIPTOR), &devContext->DeviceDescriptor, &usbDeviceDescriptor);

    if(!NT_SUCCESS(status)) 
	{
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "WdfMemoryCreate for Device Descriptor failed %!STATUS!\n", status);
        return status;
    }

    WdfUsbTargetDeviceGetDeviceDescriptor(devContext->UsbDevice, usbDeviceDescriptor);

    // Get the Interrupt pipe. There are other endpoints but we are only
    // interested in interrupt endpoint since our HID data comes from that
    // endpoint. Another way to get the interrupt endpoint is by enumerating
    // through all the pipes in a loop and looking for pipe of Interrupt type.
    devContext->InterruptPipe = WdfUsbInterfaceGetConfiguredPipe(devContext->UsbInterface, INTERRUPT_ENDPOINT_INDEX, NULL);
    if (NULL == devContext->InterruptPipe) 
	{
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "Failed to get interrupt pipe info\n");
        status = STATUS_INVALID_DEVICE_STATE;
        return status;
    }

    // Tell the framework that it's okay to read less than MaximumPacketSize
    WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(devContext->InterruptPipe);

    // Light control is optional: a failure here must not stop the controller.
    HidSteelBattalionPrepareLeds(Device, devContext);

    //configure continuous reader
    status = HidSteelBattalionConfigContReaderForInterruptEndPoint(devContext);

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT, "HidSteelBattalionEvtDevicePrepareHardware Exit, Status:0x%x\n", status);

    return status;
}


static VOID HidSteelBattalionPrepareLeds
(
    IN WDFDEVICE Device,
    IN PDEVICE_EXTENSION DeviceContext
)
/*++
Routine Description:
    Finds the output pipe for the panel lights and allocates the one request
    and buffer used to write to it. If anything fails LedPipe stays NULL and the
    lights are simply left alone.
--*/
{
    NTSTATUS              status;
    WDF_OBJECT_ATTRIBUTES attributes;
    BYTE                  pipeCount;
    BYTE                  i;

    PAGED_CODE();

    DeviceContext->LedPipe = NULL;
    DeviceContext->LedsValid = FALSE;

    if (DeviceContext->LedRequest == NULL)
    {
        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.ParentObject = Device;
        status = WdfRequestCreate(&attributes, WdfUsbTargetDeviceGetIoTarget(DeviceContext->UsbDevice), &DeviceContext->LedRequest);
        if (!NT_SUCCESS(status))
        {
            DeviceContext->LedRequest = NULL;
            TraceEvents(TRACE_LEVEL_WARNING, DBG_PNP, "WdfRequestCreate for lights failed 0x%x\n", status);
            return;
        }
    }

    if (DeviceContext->LedMemory == NULL)
    {
        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.ParentObject = Device;
        status = WdfMemoryCreate(&attributes, NonPagedPoolNx, POOL_TAG, SBC_LED_DATA_SIZE, &DeviceContext->LedMemory, (PVOID*)&DeviceContext->LedBuffer);
        if (!NT_SUCCESS(status))
        {
            DeviceContext->LedMemory = NULL;
            TraceEvents(TRACE_LEVEL_WARNING, DBG_PNP, "WdfMemoryCreate for lights failed 0x%x\n", status);
            return;
        }
    }

    pipeCount = WdfUsbInterfaceGetNumConfiguredPipes(DeviceContext->UsbInterface);
    for (i = 0; i < pipeCount; ++i)
    {
        WDFUSBPIPE pipe = WdfUsbInterfaceGetConfiguredPipe(DeviceContext->UsbInterface, i, NULL);
        if (pipe != NULL && WdfUsbTargetPipeIsOutEndpoint(pipe))
        {
            DeviceContext->LedPipe = pipe;
            break;
        }
    }

    if (DeviceContext->LedPipe == NULL)
    {
        TraceEvents(TRACE_LEVEL_WARNING, DBG_PNP, "No output pipe found for lights\n");
    }
}


static VOID HidSteelBattalionBuildLeds(IN const SBC_INPUT_DATA* Input, OUT UCHAR Leds[SBC_LED_DATA_SIZE])
/*++
Routine Description:
    Button lights stay on unless that button is held. Exactly one gear light
    is on, matching the lever. Lights 4..33 follow the button bits in order,
    so light N belongs to button bit N - 1.
--*/
{
    const UCHAR buttons[5] = { Input->Buttons0, Input->Buttons1, Input->Buttons2, Input->Buttons3, Input->Buttons4 };
    const int gear = Input->Gear;
    int led;

    RtlZeroMemory(Leds, SBC_LED_DATA_SIZE);

    for (led = SBC_LED_FIRST_BUTTON; led <= SBC_LED_LAST_BUTTON; ++led)
    {
        const int bit = led - 1;
        if ((buttons[bit / 8] & (1 << (bit % 8))) == 0)
        {
            Leds[led / 2] |= (UCHAR)((led & 1) ? (SBC_LED_MAX_BRIGHTNESS << 4) : SBC_LED_MAX_BRIGHTNESS);
        }
    }

    if (gear >= -2 && gear <= 5 && gear != 0)
    {
        led = SBC_LED_GEAR_REVERSE + (gear < 0 ? gear + 2 : gear + 1);
        Leds[led / 2] |= (UCHAR)((led & 1) ? (SBC_LED_MAX_BRIGHTNESS << 4) : SBC_LED_MAX_BRIGHTNESS);
    }
}


static VOID HidSteelBattalionUpdateLeds(IN PDEVICE_EXTENSION DeviceContext, IN const SBC_INPUT_DATA* Input)
/*++
Routine Description:
    Sends the lights to the device if they changed. Called from the reader at
    up to DISPATCH_LEVEL. Only one write runs at a time; a change that arrives
    during a write is picked up by the next report, as the device reports
    continuously.
--*/
{
    UCHAR    leds[SBC_LED_DATA_SIZE];
    NTSTATUS status;

    if (DeviceContext->LedPipe == NULL)
    {
        return;
    }

    HidSteelBattalionBuildLeds(Input, leds);

    if (InterlockedCompareExchange(&DeviceContext->LedWriteBusy, 1, 0) != 0)
    {
        return;
    }

    if (DeviceContext->LedsValid && RtlEqualMemory(leds, DeviceContext->LedBuffer, SBC_LED_DATA_SIZE))
    {
        InterlockedExchange(&DeviceContext->LedWriteBusy, 0);
        return;
    }

    // A failed write is not retried until the lights change again, so a device
    // that rejects writes can't turn this into a tight loop at the report rate.
    RtlCopyMemory(DeviceContext->LedBuffer, leds, SBC_LED_DATA_SIZE);
    DeviceContext->LedsValid = TRUE;

    status = WdfUsbTargetPipeFormatRequestForWrite(DeviceContext->LedPipe, DeviceContext->LedRequest, DeviceContext->LedMemory, NULL);
    if (NT_SUCCESS(status))
    {
        WdfRequestSetCompletionRoutine(DeviceContext->LedRequest, HidSteelBattalionEvtLedWriteComplete, DeviceContext);
        if (WdfRequestSend(DeviceContext->LedRequest, WdfUsbTargetPipeGetIoTarget(DeviceContext->LedPipe), WDF_NO_SEND_OPTIONS))
        {
            return; // The completion routine releases LedWriteBusy
        }
        TraceEvents(TRACE_LEVEL_WARNING, DBG_IOCTL, "WdfRequestSend for lights failed\n");
    }

    {
        WDF_REQUEST_REUSE_PARAMS reuse;
        WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
        WdfRequestReuse(DeviceContext->LedRequest, &reuse);
    }
    InterlockedExchange(&DeviceContext->LedWriteBusy, 0);
}


VOID HidSteelBattalionEvtLedWriteComplete
(
    IN WDFREQUEST Request,
    IN WDFIOTARGET Target,
    IN PWDF_REQUEST_COMPLETION_PARAMS Params,
    IN WDFCONTEXT Context
)
{
    PDEVICE_EXTENSION        devContext = Context;
    WDF_REQUEST_REUSE_PARAMS reuse;

    UNREFERENCED_PARAMETER(Target);

    if (!NT_SUCCESS(Params->IoStatus.Status))
    {
        TraceEvents(TRACE_LEVEL_WARNING, DBG_IOCTL, "Light write failed 0x%x\n", Params->IoStatus.Status);
    }

    WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
    WdfRequestReuse(Request, &reuse);
    InterlockedExchange(&devContext->LedWriteBusy, 0);
}


NTSTATUS HidSteelBattalionConfigContReaderForInterruptEndPoint
(
    PDEVICE_EXTENSION DeviceContext
)
/*++
Routine Description:
    This routine configures a continuous reader on the
    interrupt endpoint. It's called from the PrepareHarware event.

Arguments:
    DeviceContext - Pointer to device context structure

Return Value:
    NT status value
--*/
{
    WDF_USB_CONTINUOUS_READER_CONFIG contReaderConfig;
    NTSTATUS status = STATUS_SUCCESS;

    PAGED_CODE ();

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT, "HidSteelBattalionConfigContReaderForInterruptEndPoint Enter\n");

    WDF_USB_CONTINUOUS_READER_CONFIG_INIT(&contReaderConfig, HidSteelBattalionEvtUsbInterruptPipeReadComplete, DeviceContext, 32);
    
    // Reader requests are not posted to the target automatically.
    // Driver must explictly call WdfIoTargetStart to kick start the
    // reader.  In this sample, it's done in D0Entry.
    // By defaut, framework queues two requests to the target
    // endpoint. Driver can configure up to 10 requests with CONFIG macro.
    status = WdfUsbTargetPipeConfigContinuousReader(DeviceContext->InterruptPipe, &contReaderConfig);

    if (!NT_SUCCESS(status)) 
	{
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT, "HidSteelBattalionConfigContReaderForInterruptEndPoint failed %x\n", status);
        return status;
    }

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT, "HidSteelBattalionConfigContReaderForInterruptEndPoint Exit, status:0x%x\n", status);

    return status;
}


VOID HidSteelBattalionEvtUsbInterruptPipeReadComplete
(
    WDFUSBPIPE  Pipe,
    WDFMEMORY   Buffer,
    size_t      NumBytesTransferred,
    WDFCONTEXT  Context
 )
/*++
Routine Description:
    This the completion routine of the continuous reader. This can
    called concurrently on multiprocessor system if there are
    more than one readers configured. So make sure to protect
    access to global resources.

Arguments:
    Pipe - Handle to WDF USB pipe object

    Buffer - This buffer is freed when this call returns.
             If the driver wants to delay processing of the buffer, it
             can take an additional referrence.

    NumBytesTransferred - number of bytes of data that are in the read buffer.

    Context - Provided in the WDF_USB_CONTINUOUS_READER_CONFIG_INIT macro

Return Value:
    NT status value
--*/
{
    PDEVICE_EXTENSION  devContext = Context;
    PUCHAR             inputData = NULL;

    UNREFERENCED_PARAMETER(NumBytesTransferred);
    UNREFERENCED_PARAMETER(Pipe);

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT, "HidSteelBattalionEvtUsbInterruptPipeReadComplete Enter\n");

    if (NumBytesTransferred == 0) 
	{
        TraceEvents(TRACE_LEVEL_WARNING, DBG_INIT, "HidSteelBattalionEvtUsbInterruptPipeReadComplete Zero length read occured on the Interrupt Pipe's Continuous Reader\n");
        return;
    }

	if (NumBytesTransferred < 26)
	{
		TraceEvents(TRACE_LEVEL_WARNING, DBG_INIT, "HidSteelBattalionEvtUsbInterruptPipeReadComplete length is smaller than expected on the Interrupt Pipe's Continuous Reader\n");
		return;
	}

	inputData = WdfMemoryGetBuffer(Buffer, NULL);
	//TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT, "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
	//	inputData[0], inputData[1], inputData[2], inputData[3], inputData[4], inputData[5], inputData[6], inputData[7], inputData[8], inputData[9], inputData[10], inputData[11], inputData[12], inputData[13], inputData[14], inputData[15], inputData[16], inputData[17], inputData[18], inputData[19], inputData[20], inputData[21], inputData[22], inputData[23], inputData[24], inputData[25], inputData[26], inputData[27], inputData[28], inputData[29], inputData[30], inputData[31]);

	PSBC_INPUT_DATA sbcData = (PSBC_INPUT_DATA)inputData;

	// Before the HID queue check below: lights must track the controller even
	// when no application has the device open.
	HidSteelBattalionUpdateLeds(devContext, sbcData);

	HIDFX2_INPUT_REPORT r;
	r.Buttons0 = sbcData->Buttons0;
	r.Buttons1 = sbcData->Buttons1;
	r.Buttons2 = sbcData->Buttons2;
	r.Buttons3 = sbcData->Buttons3;
	r.Buttons4 = sbcData->Buttons4;
	r.AimX = (BYTE)(((int)sbcData->AimX) / 256);
	r.AimY = (BYTE)(((int)sbcData->AimY) / 256);
	r.Rotation = (BYTE)(((int)sbcData->Rotation) / 256 + 128);
	r.SightX = (BYTE)(((int)sbcData->SightX) / 256 + 128);
	r.SightY = (BYTE)(((int)sbcData->SightY) / 256 + 128);
	r.Clutch = (BYTE)(((int)sbcData->Clutch) / 256);
	r.Brake = (BYTE)(((int)sbcData->Brake) / 256);
	r.Throttle = (BYTE)(((int)sbcData->Throttle) / 256);

	// One button per position. The lever reports -2 (R), -1 (N) or 1..5, and the
	// dial position is the low nibble of its byte.
	{
		const int gear = sbcData->Gear;
		ULONG extra = 1UL << (7 + (sbcData->Tuner & 0x0F));
		if (gear >= -2 && gear <= 5 && gear != 0)
		{
			extra |= 1UL << (gear < 0 ? gear + 2 : gear + 1);
		}
		r.GearDialButtons[0] = (BYTE)(extra & 0xFF);
		r.GearDialButtons[1] = (BYTE)((extra >> 8) & 0xFF);
		r.GearDialButtons[2] = (BYTE)((extra >> 16) & 0xFF);
	}

	NTSTATUS status;
	WDFREQUEST request;

	status = WdfIoQueueRetrieveNextRequest(devContext->InterruptMsgQueue, &request);
	if (!NT_SUCCESS(status))
	{
		TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL, "WdfIoQueueRetrieveNextRequest status %08x\n", status);
		return;
	}

	PVOID outputBuffer;
	size_t bytesReturned;
	size_t reportSize = sizeof(r);
	status = WdfRequestRetrieveOutputBuffer(request, reportSize, &outputBuffer, &bytesReturned);
	if (NT_SUCCESS(status))
	{
		//memcpy(outputBuffer, &(devContext->CurrentInputReport), sizeof(HIDFX2_INPUT_REPORT));
		memcpy(outputBuffer, &r, reportSize);
		WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, reportSize);
	}
	else
	{
		TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL, "WdfRequestRetrieveOutputBuffer status %08x\n", status);
		WdfRequestCompleteWithInformation(request, status, 0);
	}

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT, "HidSteelBattalionEvtUsbInterruptPipeReadComplete Exit\n");
}

NTSTATUS HidSteelBattalionEvtDeviceD0Entry
(
    IN  WDFDEVICE Device,
    IN  WDF_POWER_DEVICE_STATE PreviousState
)
/*++
Routine Description:
    EvtDeviceD0Entry event callback must perform any operations that are
    necessary before the specified device is used.  It will be called every
    time the hardware needs to be (re-)initialized.

    This function is not marked pageable because this function is in the
    device power up path. When a function is marked pagable and the code
    section is paged out, it will generate a page fault which could impact
    the fast resume behavior because the client driver will have to wait
    until the system drivers can service this page fault.

    This function runs at PASSIVE_LEVEL, even though it is not paged.  A
    driver can optionally make this function pageable if DO_POWER_PAGABLE
    is set.  Even if DO_POWER_PAGABLE isn't set, this function still runs
    at PASSIVE_LEVEL.  In this case, though, the function absolutely must
    not do anything that will cause a page fault.

Arguments:
    Device - Handle to a framework device object.

    PreviousState - Device power state which the device was in most recently.
        If the device is being newly started, this will be
        PowerDeviceUnspecified.

Return Value:
    NTSTATUS
--*/
{
    PDEVICE_EXTENSION   devContext = NULL;
    NTSTATUS            status = STATUS_SUCCESS;

    devContext = GetDeviceContext(Device);

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_PNP, "HidSteelBattalionEvtDeviceD0Entry Enter - coming from %s\n", DbgDevicePowerString(PreviousState));

	// Resend the lights after any power-up, as the device may have lost them.
	devContext->LedsValid = FALSE;
	if (devContext->LedPipe != NULL)
	{
		(VOID)WdfIoTargetStart(WdfUsbTargetPipeGetIoTarget(devContext->LedPipe));
	}

	status = WdfIoTargetStart(WdfUsbTargetPipeGetIoTarget(devContext->InterruptPipe));

    TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "HidSteelBattalionEvtDeviceD0Entry Exit, status: 0x%x\n", status);

    return status;
}


NTSTATUS HidSteelBattalionEvtDeviceD0Exit
(
    IN  WDFDEVICE Device,
    IN  WDF_POWER_DEVICE_STATE TargetState
)
/*++
Routine Description:
    This routine undoes anything done in EvtDeviceD0Entry.  It is called
    whenever the device leaves the D0 state, which happens when the device is
    stopped, when it is removed, and when it is powered off.

    The device is still in D0 when this callback is invoked, which means that
    the driver can still touch hardware in this routine.


    EvtDeviceD0Exit event callback must perform any operations that are
    necessary before the specified device is moved out of the D0 state.  If the
    driver needs to save hardware state before the device is powered down, then
    that should be done here.

    This function runs at PASSIVE_LEVEL, though it is generally not paged.  A
    driver can optionally make this function pageable if DO_POWER_PAGABLE is set.

    Even if DO_POWER_PAGABLE isn't set, this function still runs at
    PASSIVE_LEVEL.  In this case, though, the function absolutely must not do
    anything that will cause a page fault.

Arguments:
    Device - Handle to a framework device object.

    TargetState - Device power state which the device will be put in once this
        callback is complete.

Return Value:
    Success implies that the device can be used.  Failure will result in the
    device stack being torn down.
--*/
{
    PDEVICE_EXTENSION         devContext;

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_PNP, "HidSteelBattalionEvtDeviceD0Exit Enter- moving to %s\n", DbgDevicePowerString(TargetState));

    devContext = GetDeviceContext(Device);
    // Reader first, so no new light writes start once the light pipe is stopped.
    WdfIoTargetStop(WdfUsbTargetPipeGetIoTarget(devContext->InterruptPipe), WdfIoTargetCancelSentIo);
    if (devContext->LedPipe != NULL)
    {
        WdfIoTargetStop(WdfUsbTargetPipeGetIoTarget(devContext->LedPipe), WdfIoTargetCancelSentIo);
    }

    TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "HidSteelBattalionEvtDeviceD0Exit Exit\n");

    return STATUS_SUCCESS;
}

USBD_STATUS HidSteelBattalionValidateConfigurationDescriptor
(
    IN PUSB_CONFIGURATION_DESCRIPTOR ConfigDesc,
    IN ULONG BufferLength,
    _Inout_ PUCHAR *Offset
)
/*++
Routine Description:
    Validates a USB Configuration Descriptor

Parameters:
    ConfigDesc: Pointer to the entire USB Configuration descriptor returned by the device

    BufferLength: Known size of buffer pointed to by ConfigDesc (Not wTotalLength)

    Offset: if the USBD_STATUS returned is not USBD_STATUS_SUCCESS, offet will
        be set to the address within the ConfigDesc buffer where the failure occured.

Return Value:
    USBD_STATUS
    Success implies the configuration descriptor is valid.
--*/
{
    USBD_STATUS status = USBD_STATUS_SUCCESS;
    USHORT ValidationLevel = 3;

    PAGED_CODE();

    // Call USBD_ValidateConfigurationDescriptor to validate the descriptors which are present in this supplied configuration descriptor.
    // USBD_ValidateConfigurationDescriptor validates that all descriptors are completely contained within the configuration descriptor buffer.
    // It also checks for interface numbers, number of endpoints in an interface etc.
    // Please refer to msdn documentation for this function for more information.
    status = USBD_ValidateConfigurationDescriptor(ConfigDesc, BufferLength , ValidationLevel , Offset , POOL_TAG);
    if (!(NT_SUCCESS(status))) return status;

    // TODO: You should validate the correctness of other descriptors which are not taken care by USBD_ValidateConfigurationDescriptor
    // Check that all such descriptors have size >= sizeof(the descriptor they point to)
    // Check for any association between them if required

    return status;
}


PCHAR DbgDevicePowerString(IN WDF_POWER_DEVICE_STATE Type)
{
    switch (Type)
    {
    case WdfPowerDeviceInvalid:					return "WdfPowerDeviceInvalid";
    case WdfPowerDeviceD0:						return "WdfPowerDeviceD0";
    case WdfPowerDeviceD1:						return "WdfPowerDeviceD1";
    case WdfPowerDeviceD2:						return "WdfPowerDeviceD2";
    case WdfPowerDeviceD3:						return "WdfPowerDeviceD3";
    case WdfPowerDeviceD3Final:					return "WdfPowerDeviceD3Final";
    case WdfPowerDevicePrepareForHibernation:	return "WdfPowerDevicePrepareForHibernation";
    case WdfPowerDeviceMaximum:					return "WdfPowerDeviceMaximum";
    default:									return "UnKnown Device Power State";
    }
}
