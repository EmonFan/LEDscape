/** \file
 * Userspace interface to the WS281x LED strip driver.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <prussdrv.h>
#include <pruss_intc_mapping.h>

#define PRU_NUM  (0)

// Should get this from /sys/class/uio/uio0/maps/map1/addr
//#define DDR_BASEADDR 0x80000000
#define DDR_BASEADDR 0x99400000
#define OFFSET_DDR	 0x00001000 
#define OFFSET_L3	 2048       //equivalent with 0x00002000

#define die(fmt, ...) \
	do { \
		fprintf(stderr, fmt, ## __VA_ARGS__); \
		exit(EXIT_FAILURE); \
	} while (0)

typedef struct
{
	// in the DDR shared with the PRU
	const void * pixels;

	// in bytes of the entire pixel array.
	// Should be Num pixels * Num strips * 3
	unsigned size;

	// write 1 to start, 0xFF to abort. will be cleared when started
	volatile unsigned command;

	// will have a non-zero response written when done
	volatile unsigned response;
} ws281x_command_t;

//static ws281x_command_t * ws281x_command; // mapped to the PRU DRAM
static uint8_t * pixels; // mapped to the L3 shared with the PRU

static unsigned int
proc_read(
	const char * const fname
)
{
	FILE * const f = fopen(fname, "r");
	if (!f)
		die("%s: Unable to open: %s", fname, strerror(errno));
	unsigned int x;
	fscanf(f, "%x", &x);
	fclose(f);
	return x;
}


static ws281x_command_t *
ws281_init(
	unsigned short pruNum,
	unsigned num_leds
)
{
	void * pruDataMem;
	prussdrv_map_prumem(
		pruNum == 0 ? PRUSS0_PRU0_DATARAM :PRUSS0_PRU1_DATARAM,
		&pruDataMem
	);

	const int mem_fd = open("/dev/mem", O_RDWR);
	if (mem_fd < 0)
		die("Failed to open /dev/mem: %s\n", strerror(errno));

	const uintptr_t ddr_addr = proc_read("/sys/class/uio/uio0/maps/map1/addr");
	const uintptr_t ddr_size = proc_read("/sys/class/uio/uio0/maps/map1/size");

	const uintptr_t ddr_start = 0x10000000;
	const uintptr_t ddr_offset = ddr_addr - ddr_start;
	const size_t ddr_filelen = ddr_size + ddr_start;

	/* map the memory */
	uint8_t * const ddr_mem = mmap(
		0,
		ddr_filelen,
		PROT_WRITE | PROT_READ,
		MAP_SHARED,
		mem_fd,
		ddr_offset
	);
	if (ddr_mem == MAP_FAILED)
		die("Failed to mmap offset %"PRIxPTR" @ %zu bytes: %s\n",
			ddr_offset,
			ddr_filelen,
			strerror(errno)
		);
    
    	ws281x_command_t * const cmd = (void*) pruDataMem;
	cmd->pixels = (void*) ddr_addr;
	cmd->size = num_leds;
	cmd->command = 0;
	cmd->response = 0;

	const size_t pixel_size = num_leds * 32 * 4;

	if (pixel_size > ddr_size)
		die("Pixel data needs at least %zu, only %zu in DDR\n",
			pixel_size,
			ddr_size
		);

#if 0
	prussdrv_map_l3mem (&l3mem);	
	pixels = l3mem;
#else
	pixels = ddr_mem + ddr_start;
#endif

	// Store values into source
	printf("data ram %p l3 ram %p: setting %zu bytes\n",
		cmd,
		pixels,
		pixel_size
	);

	size_t i;
	for(i=0 ; i < pixel_size ; i++)
		pixels[i] = ((i * 13) / 17) & 0xFF;

	return cmd;
}


int main (void)
{
    prussdrv_init();		

    int ret = prussdrv_open(PRU_EVTOUT_0);
    if (ret)
        die("prussdrv_open open failed\n");

    tpruss_intc_initdata pruss_intc_initdata = PRUSS_INTC_INITDATA;
    prussdrv_pruintc_init(&pruss_intc_initdata);

    ws281x_command_t * cmd = ws281_init(PRU_NUM, 256);

    prussdrv_exec_program (PRU_NUM, "./ws281x.bin");

    int i;
    for (i = 0 ; i < 16 ; i++)
    {
	printf("starting %d!\n", i);
	cmd->response = 0;
	cmd->command = 1;
	while (!cmd->response)
		;
	const uint32_t * next = (uint32_t*)(cmd + 1);
	printf("done! %08x %08x\n", cmd->response, *next);
    }

    // Signal a halt command
    cmd->command = 0xFF;

    prussdrv_pru_wait_event(PRU_EVTOUT_0);
    prussdrv_pru_clear_event(PRU0_ARM_INTERRUPT);
    prussdrv_pru_disable(PRU_NUM); 
    prussdrv_exit();

    return EXIT_SUCCESS;
}

