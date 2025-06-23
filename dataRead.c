/*
 * Test Code for reading from a USB port with LabJack T7
 * libusb
 * Must change global variables depending on device
 */

#include <stdio.h>
#include <stdlib.h>
#include <libusb-1.0/libusb.h>
#include <string.h>

#define VENDOR_ID 0x2040
#define PRODUCT_ID 0xc200
#define ENDPOINT 0x84
#define INTERFACE 3

#define TRANSFER_SIZE 64
#define NUM_TRANSFERS 8

libusb_context *ctx = NULL;
libusb_device_handle *handle = NULL;

void LIBUSB_CALL callback(struct libusb_transfer *transfer) {
	if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
		printf("%d bytes: ", transfer->actual_length);
		for (int i = 0; i < transfer->actual_length; ++i) {
			printf("%02x ", transfer->buffer[i]);
		}
		printf("\n");

		libusb_submit_transfer(transfer);
	}
	else {
		fprintf(stderr, "Transfer error %s\n", libusb_error_name(transfer->status));
		libusb_free_transfer(transfer);
	}

}

int main()
{
	libusb_context *ctx = NULL;
	libusb_device_handle *handle = NULL;
	int r;
	
	// Init libusb with checking
	r = libusb_init(&ctx);
	if (r < 0) {
		fprintf(stderr, "Failed to init libusb: %s\n", libusb_error_name(r));
		return 1;
	}
	
	libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_INFO);

	// Open USB device
	handle = libusb_open_device_with_vid_pid(ctx, VENDOR_ID, PRODUCT_ID);
	if(!handle) {
		fprintf(stderr, "Could not find/open device VENDOR_ID:%04x PRODUCT_ID::%04x\n", VENDOR_ID, PRODUCT_ID);
		libusb_exit(ctx);
		return 1;
	}
	printf("Connected device (%04x:%04x)\n", VENDOR_ID, PRODUCT_ID);
	
	// Detach Kernel drivers
	// Kernel sometimes auto claim USB devices
	if (libusb_kernel_driver_active(handle, INTERFACE)) {
		printf("USB busy... Detaching Kernel driver\n");
		r = libusb_detach_kernel_driver(handle, INTERFACE);
		if (r != 0) {
			fprintf(stderr, "Failed to detach kernel driver: %s\n", libusb_error_name(r));
			libusb_close(handle);
			libusb_exit(ctx);
			return 1;
		}
	}

	// Claim interface
	r = libusb_claim_interface(handle, INTERFACE);
	if ( r != 0) {
		fprintf(stderr, "Failed to claim interface: %s\n", libusb_error_name(r));
		libusb_close(handle);
		libusb_exit(ctx);
		return 1;
	}
	printf("Successfully claimed interface %d\n", INTERFACE);
	
	unsigned char buffer[TRANSFER_SIZE];
	int actual_length = 0;
	
	r = libusb_bulk_transfer(handle, ENDPOINT, buffer, TRANSFER_SIZE, &actual_length, 1000);
	if (r == 0) {
	  printf("Sync read %d bytes:\n", actual_length);
	  for (int i = 0; i < actual_length; i++) {
	    printf("%02x ", buffer[i]);
	  }
	  printf("\n");
	}
	else {
	  fprintf(stderr, "Bulk transfer failed: %s\n", libusb_error_name(r));
	  libusb_release_interface(handle, INTERFACE);
	  libusb_close(handle);
	  libusb_exit(ctx);
	  return 1;
	}
	
	for (int i = 0; i < NUM_TRANSFERS; ++i){
		struct libusb_transfer *transfer = libusb_alloc_transfer(0);
		unsigned char *buffer = malloc(TRANSFER_SIZE);
		memset(buffer, 0, TRANSFER_SIZE);

		libusb_fill_bulk_transfer(transfer, handle, ENDPOINT, buffer, TRANSFER_SIZE, callback, NULL, 1000);
		
		r = libusb_submit_transfer(transfer);
		if (r != 0){
			fprintf(stderr, "Failed to submit transfer: %s\n", libusb_error_name(r));
			free(buffer);
			libusb_free_transfer(transfer);
		}
	}

	printf("Streaming data started!");

	while (1){
		r = libusb_handle_events(ctx);
		if (r < 0){
			fprintf(stderr, "Event handling error: %s\n", libusb_error_name(r));
			break;
		}
	}

	// Cleanup
	libusb_release_interface(handle, INTERFACE);
	libusb_close(handle);
	libusb_exit(ctx);

return 0;
}




