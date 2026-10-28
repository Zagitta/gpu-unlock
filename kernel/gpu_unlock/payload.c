// SPDX-License-Identifier: GPL-2.0-only
/* Signature image construction. The image is data the signed Booter reads, so
 * every offset is validated against the selected descriptor before writing. */
#include "diagnostic.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

#include "payload.h"

static void put_word(u8 *image, u32 offset, u32 value)
{
	unsigned int i;

	for (i = 0; i < 4; i++)
		image[offset + i] = value >> (8 * i);
}

int gu_payload_check(const struct gu_payload *payload)
{
	size_t i;

	if (!payload || !payload->name || !payload->size || (payload->size & 3) ||
	    !payload->writes || payload->writes > GU_PAYLOAD_MAX_WRITES ||
	    !payload->slots || (payload->word_count && !payload->words))
		return GU_DIAG(-EINVAL);
	for (i = 0; i < payload->writes; i++)
		if ((payload->slots[i].value_off &&
		     payload->slots[i].value_off > payload->size - 4) ||
		    (payload->slots[i].address_off &&
		     payload->slots[i].address_off > payload->size - 4))
			return GU_DIAG(-EINVAL);
	for (i = 0; i < payload->word_count; i++)
		if (payload->words[i].offset > payload->size - 4)
			return GU_DIAG(-EINVAL);
	return 0;
}

int gu_payload_build(const struct gu_payload *payload, u8 *image,
		     const struct gu_reg_write *writes, size_t count)
{
	u32 offset;
	size_t i;
	int ret;

	if (!image)
		return GU_DIAG(-EINVAL);
	ret = gu_payload_check(payload);
	if (ret)
		return ret;
	if (count > payload->writes || (count && !writes))
		return GU_DIAG(-EINVAL);
	for (offset = 0; offset < payload->size; offset += 4)
		put_word(image, offset, payload->fill);
	for (i = 0; i < payload->word_count; i++)
		put_word(image, payload->words[i].offset, payload->words[i].value);
	for (i = 0; i < count; i++) {
		if (payload->slots[i].value_off)
			put_word(image, payload->slots[i].value_off, writes[i].value);
		if (payload->slots[i].address_off)
			put_word(image, payload->slots[i].address_off, writes[i].addr);
	}
	return 0;
}
