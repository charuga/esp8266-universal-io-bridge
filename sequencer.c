#include "sequencer.h"
#include "time.h"
#include "io.h"
#include "dispatch.h"

typedef struct
{
	_Bool		flash_valid;
	int			start;
	int			current;
	uint64_t	current_end_time;
	int			repeats;
} sequencer_t;

static sequencer_t sequencer;

typedef struct
{
	union
	{
		struct
		{
			unsigned int active:1;
			unsigned int io:4;
			unsigned int pin:4;
			unsigned int duration:23;
			unsigned int value:32;
		};
		struct
		{
			unsigned int magic:32;
			unsigned int version:32;
		};
		struct
		{
			uint32_t word[2];
		};
	};
} sequencer_entry_t;

assert_size(sequencer_entry_t, 8);

enum
{
	sequencer_flash_magic = 0x4afc4afb,
	sequencer_flash_version = 0,
	sequencer_flash_sectors = 4,
	sequencer_flash_size = sequencer_flash_sectors * SPI_FLASH_SEC_SIZE, // 4 sectors of 4k bytes each
	sequencer_flash_entries = sequencer_flash_size / sizeof(sequencer_entry_t),
	sequencer_flash_entries_per_sector = sequencer_flash_entries / sequencer_flash_sectors,
	sequencer_flash_memory_map_start = 0x40200000,
};

_Static_assert(sequencer_flash_entries == 2048, "flash sequencer size incorrect");
_Static_assert(sequencer_flash_entries_per_sector == 512, "flash sequencer per sector size incorrect");

static _Bool clear_all_flash_entries(unsigned int mirror)
{
	sequencer_entry_t *entry;
	unsigned int offset, sector, current = 0;
	char *buffer;

	if(mirror > 1)
		return(false);

	if(mirror == 0)
		offset = SEQUENCER_FLASH_OFFSET_0;
	else
		offset = SEQUENCER_FLASH_OFFSET_1;

	if(offset == 0) // plain image, no mirror offset
		return(true);

	if(string_size(&flash_sector_buffer) < SPI_FLASH_SEC_SIZE)
		return(false);

	buffer = string_buffer_nonconst(&flash_sector_buffer);

	for(sector = 0; sector < sequencer_flash_sectors; sector++)
	{
		entry = (sequencer_entry_t *)(void *)buffer;

		if(sector == 0)
		{
			entry->magic = sequencer_flash_magic;
			entry->version = sequencer_flash_version;
			entry++;
		}

		for(; ((char *)entry - buffer) < SPI_FLASH_SEC_SIZE; entry++)
		{

			entry->active = 0;
			entry->io = 0;
			entry->pin = 0;
			entry->duration = 0;
			entry->value = current++;
		}

		log("sequencer clear: offset: %x, sector %u, entries written to flash: %u, entryp = %u\n",
				offset + (sector * SPI_FLASH_SEC_SIZE),
				sector,
				current,
				(char *)entry - buffer);

		if(spi_flash_erase_sector((offset + (sector * SPI_FLASH_SEC_SIZE)) / SPI_FLASH_SEC_SIZE) != SPI_FLASH_RESULT_OK)
			return(false);

		if(spi_flash_write(offset + (sector * SPI_FLASH_SEC_SIZE), buffer, SPI_FLASH_SEC_SIZE) != SPI_FLASH_RESULT_OK)
			return(false);
	}

	return(true);
}

static _Bool get_flash_entry(unsigned int index, sequencer_entry_t *entry)
{
	const sequencer_entry_t *entries_in_flash;

	if(index >= sequencer_flash_entries)
		return(false);

	// note: this will always use either mirror 0 or mirror 1 depending on which image/slot is loaded, due to the flash mapping window
	entries_in_flash = (const sequencer_entry_t *)(sequencer_flash_memory_map_start + SEQUENCER_FLASH_OFFSET);

	// careful to only read complete 32 bits words from mapped flash
	entry->word[0] = entries_in_flash[index].word[0];
	entry->word[1] = entries_in_flash[index].word[1];

	return(true);
}

static _Bool update_flash_entry(unsigned int index, unsigned int mirror, const sequencer_entry_t *entry)
{
	sequencer_entry_t *entries_in_buffer, *entry_in_buffer;
	unsigned int flash_start_offset, sector;
	char *buffer;

	if(!sequencer.flash_valid)
		return(false);

	if(index >= sequencer_flash_entries)
		return(false);

	if(mirror > 1)
		return(false);

	if(string_size(&flash_sector_buffer) < SPI_FLASH_SEC_SIZE)
		return(false);

	buffer = string_buffer_nonconst(&flash_sector_buffer);

	if(mirror == 0)
		flash_start_offset = SEQUENCER_FLASH_OFFSET_0;
	else
		flash_start_offset = SEQUENCER_FLASH_OFFSET_1;

	if(flash_start_offset == 0) // plain image, no mirror offset
		return(true);

	sector = (index * sizeof(sequencer_entry_t)) / SPI_FLASH_SEC_SIZE;

	log("update flash entry: update entry: %u, sector: %u, offset index: %u, flash start offset: %x\n",
			index,
			sector,
			index - (sector * sequencer_flash_entries_per_sector),
			flash_start_offset);

	if(spi_flash_read(flash_start_offset + (sector * SPI_FLASH_SEC_SIZE), buffer, SPI_FLASH_SEC_SIZE) != SPI_FLASH_RESULT_OK)
		return(false);

	entries_in_buffer = (sequencer_entry_t *)(void *)buffer;
	entry_in_buffer = &entries_in_buffer[index - (sector * sequencer_flash_entries_per_sector)];

	log("* buffer offset: %u\n", (char *)&entries_in_buffer[index - (sector * sequencer_flash_entries_per_sector)] - buffer);
	log("* entry1: io: %u, pin: %u, duration: %u, value: %u\n", entry_in_buffer->io, entry_in_buffer->pin, entry_in_buffer->duration, entry_in_buffer->value);

	*entry_in_buffer = *entry;

	log("* entry2: io: %u, pin: %u, duration: %u, value: %u\n", entry_in_buffer->io, entry_in_buffer->pin, entry_in_buffer->duration, entry_in_buffer->value);

	if(spi_flash_erase_sector((flash_start_offset + (sector * SPI_FLASH_SEC_SIZE)) / SPI_FLASH_SEC_SIZE) != SPI_FLASH_RESULT_OK)
		return(false);

	if(spi_flash_write(flash_start_offset + (sector * SPI_FLASH_SEC_SIZE), buffer, SPI_FLASH_SEC_SIZE) != SPI_FLASH_RESULT_OK)
		return(false);

	return(true);
}

attr_pure int sequencer_get_start(void)
{
	return(sequencer.start);
}

attr_pure int sequencer_get_current(void)
{
	return(sequencer.current);
}

iram attr_pure uint64_t sequencer_get_current_end_time(void)
{
	return(sequencer.current_end_time);
}

iram attr_pure int sequencer_get_repeats(void)
{
	return(sequencer.repeats);
}

void sequencer_get_status(_Bool *running, unsigned int *start, unsigned int *flash_size, unsigned int *flash_size_entries,
		unsigned int *flash_offset_flash0, unsigned int *flash_offset_flash1, unsigned int *flash_offset_mapped)
{
	*running = sequencer.repeats > 0;
	*start = sequencer.start;
	*flash_size = sequencer_flash_sectors * SPI_FLASH_SEC_SIZE;
	*flash_size_entries = sequencer_flash_entries;
	*flash_offset_flash0 = SEQUENCER_FLASH_OFFSET_0;
	*flash_offset_flash1 = SEQUENCER_FLASH_OFFSET_1;
	*flash_offset_mapped  = sequencer_flash_memory_map_start + SEQUENCER_FLASH_OFFSET;
}

_Bool sequencer_clear(void)
{
	if(!clear_all_flash_entries(0))		// OTA image #0 or plain image
		return(false);
	if(!clear_all_flash_entries(1))		// OTA image #1
		return(false);

	sequencer_init();

	return(sequencer.flash_valid);
}

_Bool sequencer_get_entry(unsigned int index, _Bool *active, int *io, int *pin, uint32_t *value, int *duration)
{
	sequencer_entry_t entry;

	if(!sequencer.flash_valid)
		return(false);

	index++; // index = 0 is header

	if(index >= sequencer_flash_entries)
		return(false);

	if(!get_flash_entry(index, &entry))
		return(false);

	if(active)
		*active = entry.active;

	if(io)
		*io = entry.io;

	if(pin)
		*pin = entry.pin;

	if(duration)
		*duration = entry.duration;

	if(value)
		*value = entry.value;

	return(true);
}

_Bool sequencer_set_entry(unsigned int index, int io, int pin, uint32_t value, int duration)
{
	sequencer_entry_t entry;

	if(!sequencer.flash_valid)
		return(false);

	index++;

	if(index >= sequencer_flash_entries)
		return(false);

	entry.active = 1;
	entry.io = io;
	entry.pin = pin;
	entry.duration = duration;
	entry.value = value;

	if(!update_flash_entry(index, 0, &entry))
		return(false);

	return(update_flash_entry(index, 1, &entry));
}

_Bool sequencer_remove_entry(unsigned int index)
{
	sequencer_entry_t entry;

	if(!sequencer.flash_valid)
		return(false);

	index++;

	if(index >= sequencer_flash_entries)
		return(false);

	entry.active = 0;
	entry.io = 0;
	entry.pin = 0;
	entry.duration = 0;
	entry.value = 0;

	if(!update_flash_entry(index, 0, &entry))
		return(false);

	return(update_flash_entry(index, 1, &entry));
}

void sequencer_init(void)
{
	sequencer_entry_t header;

	sequencer_stop();

	sequencer.flash_valid = 0;

	if(get_flash_entry(0, &header) && (header.magic == sequencer_flash_magic) && (header.version == sequencer_flash_version))
		sequencer.flash_valid = 1;
}

void sequencer_start(unsigned int start, unsigned int repeats)
{
	sequencer.start = start;
	sequencer.current = sequencer.start - 1;
	sequencer.current_end_time = 0;
	sequencer.repeats = repeats;
}

void sequencer_stop(void)
{
	sequencer.start = 0;
	sequencer.current = -1;
	sequencer.current_end_time = 0;
	sequencer.repeats = 0;
}

void sequencer_run(void)
{
	unsigned int io, pin, duration;
	uint32_t value;
	_Bool active;

	sequencer.current++;

	if(!sequencer_get_entry(sequencer.current, &active, &io, &pin, &value, &duration) || !active)
	{
		if(--sequencer.repeats <= 0)
		{
			sequencer_stop();
			return;
		}

		sequencer.current = sequencer.start;

		if(!sequencer_get_entry(sequencer.current, &active, &io, &pin, &value, &duration) || !active)
		{
			sequencer_stop();
			return;
		}
	}

	sequencer.current_end_time = (time_get_us() / 1000) + duration;

	io_write_pin((string_t *)0, io, pin, value);
}
