
#include <BC/Utils.h>
#include <BC/Base64.h>

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// base64 Utilities
///////////////////////////////////////////////////////////////////////////////

const char * alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int32_t base64_encode(char * dest, uint8_t * src, int32_t srcLen)
{
	int32_t a;
	uint8_t * p = src;
	int32_t kL = (int32_t)(srcLen / 3) * 3;
	char * oldDest = dest;

	while (p < src + kL)
	{
		a = (*p) >> 2;
		*dest++ = alphabet[a];

		a = ((*p) & 0x3) << 4;
		p++;

		a += (*p >> 4) ;
		*dest++ = alphabet[a];

		a = ((*p) & 0xf)<<2 ;
		p++;
		a += ((*p) >> 6);

		*dest++ = alphabet[a];

		a = (*p) & 0x3f;
		*dest++ = alphabet[a];
		p++;
	}
	int32_t n = srcLen - kL;

	if (n == 1)
	{
		a = (*p) >> 2;
		*dest++ = alphabet[a];

		a = ((*p) & 0x3) << 4;
		*dest++ = alphabet[a];
		*dest++ = '=';
		*dest++ = '=';
	}
	else if (n == 2)
	{
		a = (*p) >> 2;
		*dest++ = alphabet[a];

		a = ((*p) & 0x3) << 4;
		p++;
		a += (*p >> 4) ;
		*dest++ = alphabet[a];
		a = ((*p) & 0xf)<<2 ;
		*dest++ = alphabet[a];
		*dest++ = '=';
	}
	//*dest++ = '\n'; // ´ËÐÐ´æÒÉ
	*dest = '\0';

	return int32_t(dest - oldDest);
}


static inline int32_t __get_index(char t)
{
	if (t >= 'A' && t <= 'Z')
	{
		return t - 'A';
	}
	else if (t >= 'a' && t <= 'z')
	{
		return t - 'a' + 26;
	}
	else if (t >= '0' && t <= '9')
	{
		return t - '0' + 52;
	}
	else if (t == '+')
	{
		return 62;
	}
	else   // t == '/'
	{
		return 63;
	}
}

int32_t base64_decode(uint8_t * dest, char * src, size_t srcLen)
{
	uint8_t * oldDest = dest;
	int32_t a, b, c, d;
	char * p = src, *end = src + srcLen;

	while (*p != '\n' && *p != '\r' && *p != '\0' && p < end)
	{
		a = __get_index(*p++);
		b = __get_index(*p++);
		*dest++ = (a << 2) + (b >> 4);

		if (*p == '=')
		{
			break;
		}
		c = __get_index(*p++);
		*dest++ = ((b & 0xf) << 4) + (c >> 2);
		if (*p == '=')
		{
			break;
		}
		d = __get_index(*p++);
		*dest++ = ((c & 0x3) << 6) + d;
	}
	return int32_t(dest - oldDest);
}


static char base64DecodeTable[256];

static void initBase64DecodeTable()
{
	int i;
	for (i = 0; i < 256; ++i)
		base64DecodeTable[i] = (char)0x80;
		// default value: invalid

	for (i = 'A'; i <= 'Z'; ++i) base64DecodeTable[i] = 0 + (i - 'A');
	for (i = 'a'; i <= 'z'; ++i) base64DecodeTable[i] = 26 + (i - 'a');
	for (i = '0'; i <= '9'; ++i) base64DecodeTable[i] = 52 + (i - '0');
	base64DecodeTable[(unsigned char)'+'] = 62;
	base64DecodeTable[(unsigned char)'/'] = 63;
	base64DecodeTable[(unsigned char)'='] = 0;
}

unsigned char* Base64Decode(
	const char* lpIn,
	uint32_t & resultSize,
	bool trimTrailingZeros)
{
	static bool haveInitedBase64DecodeTable = false;
	if (!haveInitedBase64DecodeTable)
	{
		initBase64DecodeTable();
		haveInitedBase64DecodeTable = true;
	}

	unsigned char* out = (unsigned char*)strdup(lpIn); // ensures we have enough space
	int k = 0;
	int const jMax = strlen(lpIn) - 3;
	// in case "in" is not a multiple of 4 bytes (although it should be)
	for (int j = 0; j < jMax; j += 4)
	{
		char inTmp[4], outTmp[4];
		for (int i = 0; i < 4; ++i)
		{
			inTmp[i] = lpIn[i+j];
			outTmp[i] = base64DecodeTable[(unsigned char)inTmp[i]];
			if ((outTmp[i]&0x80) != 0)
				outTmp[i] = 0; // pretend the input was 'A'
		}

		out[k++] = (outTmp[0]<<2) | (outTmp[1]>>4);
		out[k++] = (outTmp[1]<<4) | (outTmp[2]>>2);
		out[k++] = (outTmp[2]<<6) | outTmp[3];
	}

	if (trimTrailingZeros)
	{
		while (k > 0 && out[k-1] == '\0') --k;
	}
	resultSize = k;
	unsigned char* result = new unsigned char[resultSize];
	memmove(result, out, resultSize);
	free(out);

	return result;
}

static const char base64Char[] =
"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char* Base64Encode(const void *origSigned, uint32_t origLength)
{
	unsigned char const* orig = (unsigned char const*)origSigned; // in case any input bytes have the MSB set
	if (orig == NULL)
		return NULL;

	uint32_t const numOrig24BitValues = origLength/3;
	bool havePadding = origLength > numOrig24BitValues*3;
	bool havePadding2 = origLength == numOrig24BitValues*3 + 2;
	uint32_t const numResultBytes = 4*(numOrig24BitValues + havePadding);
	char* result = new char[numResultBytes+1]; // allow for trailing '\0'

	// Map each full group of 3 input bytes into 4 output base-64 characters:
	uint32_t i;
	for (i = 0; i < numOrig24BitValues; ++i)
	{
		result[4*i+0] = base64Char[(orig[3*i]>>2)&0x3F];
		result[4*i+1] = base64Char[(((orig[3*i]&0x3)<<4) | (orig[3*i+1]>>4))&0x3F];
		result[4*i+2] = base64Char[((orig[3*i+1]<<2) | (orig[3*i+2]>>6))&0x3F];
		result[4*i+3] = base64Char[orig[3*i+2]&0x3F];
	}

	// Now, take padding into account.  (Note: i == numOrig24BitValues)
	if (havePadding)
	{
		result[4*i+0] = base64Char[(orig[3*i]>>2)&0x3F];
		if (havePadding2)
		{
			result[4*i+1] = base64Char[(((orig[3*i]&0x3)<<4) | (orig[3*i+1]>>4))&0x3F];
			result[4*i+2] = base64Char[(orig[3*i+1]<<2)&0x3F];
		}
		else
		{
			result[4*i+1] = base64Char[((orig[3*i]&0x3)<<4)&0x3F];
			result[4*i+2] = '=';
		}
		result[4*i+3] = '=';
	}

	result[numResultBytes] = '\0';
	return result;
}

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC


///////////////////////////////////////////////////////////////////////////////
// End of file...
///////////////////////////////////////////////////////////////////////////////
