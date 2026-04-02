///////////////////////////////////////////////////////////////////////////////
// file : StringUtils.cpp
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////

#include <time.h>
#include <BC/Utils.h>
#include <HTTP/StringUtils.h>



///////////////////////////////////////////////////////////////////////////////
// Namespace : HTTP
///////////////////////////////////////////////////////////////////////////////

namespace HTTP
{

///////////////////////////////////////////////////////////////////////////////
// Class : StringFormatter
///////////////////////////////////////////////////////////////////////////////

const char	*	StringFormatter::sEOL = "\r\n";
uint32_t		StringFormatter::sEOLLen = 2;

void StringFormatter::Put(const int32_t num)
{
    char buff[32];
    sprintf(buff, "%" _S32BITARG_, num);
    Put(buff);
}

void StringFormatter::Put(const char* buffer, uint32_t bufferSize)
{
	//optimization for writing 1 character
    if((bufferSize == 1) && (fCurrentPut != fEndPut))
	{
        *(fCurrentPut++) = *buffer;
        fBytesWritten++;
        return;
    }

    //loop until the input buffer size is smaller than the space in the output
    //buffer. Call BufferIsFull at each pass through the loop
    uint32_t spaceLeft = this->GetSpaceLeft();
    uint32_t spaceInBuffer =  spaceLeft - 1;
    uint32_t resizedSpaceLeft = 0;

    while ( (spaceInBuffer < bufferSize) || (spaceLeft == 0) ) // too big for destination
    {
        if (spaceLeft > 0)
        {
			//copy as much as possible; truncating the result
            ::memcpy(fCurrentPut, buffer, spaceInBuffer);
            fCurrentPut += spaceInBuffer;
            fBytesWritten += spaceInBuffer;
            buffer += spaceInBuffer;
            bufferSize -= spaceInBuffer;
        }
        this->BufferIsFull(fStartPut, this->GetCurrentOffset()); // resize buffer
        resizedSpaceLeft = this->GetSpaceLeft();
        if (spaceLeft == resizedSpaceLeft) // couldn't resize, nothing left to do
        {
           return; // done. There is either nothing to do or nothing we can do because the BufferIsFull
        }
        spaceLeft = resizedSpaceLeft;
        spaceInBuffer =  spaceLeft - 1;
    }

    //copy the remaining chunk into the buffer
    ::memcpy(fCurrentPut, buffer, bufferSize);
    fCurrentPut += bufferSize;
    fBytesWritten += bufferSize;

}

//Puts a printf-style formatted string; except that the NUL terminator is not written.  If the buffer is too small, returns false and does not
//Alter the buffer.  Will not count the '\0' terminator as among the bytes written
bool StringFormatter::PutFmtStr(const char *fmt,  ...)
{
	ASSERT(fmt != NULL);

    va_list args;
	for(;;)
	{
		va_start(args,fmt);
#if _MSC_VER < 1300
		int length = ::vsnprintf(fCurrentPut, this->GetSpaceLeft(), fmt, args);
#elif _MSC_VER >= 1300	// MSVC7
		int length = ::_vsnprintf(fCurrentPut, this->GetSpaceLeft(), fmt, args);
#endif
		va_end(args);

		if (length < 0)
			return false;
		if (static_cast<uint32_t>(length) >= this->GetSpaceLeft()) //was not able to write all the output
		{
			if (this->BufferIsFull(fStartPut, this->GetCurrentOffset()))
				continue;
			//can only output a portion of the string
			uint32_t bytesWritten = fEndPut - fCurrentPut - 1; //We don't want to include the NUL terminator
			fBytesWritten += bytesWritten;
			fCurrentPut += bytesWritten;
			return false;
		}
		else
		{
			fBytesWritten += length;
			fCurrentPut += length;
		}
		return true;
	}
}

///////////////////////////////////////////////////////////////////////////////
// Class : ResizableStringFormatter
///////////////////////////////////////////////////////////////////////////////

void ResizeableStringFormatter::Reset(char* inBuffer /* = NULL */, uint32_t inBufSize /* = 0 */)
{
	fCurrentPut = inBuffer;
	fStartPut = inBuffer;
	fEndPut = inBuffer + inBufSize;
	fBytesWritten = 0;
}

void ResizeableStringFormatter::Reset(BCStrPtrLen &refBuffer)
{
	fCurrentPut = refBuffer.Ptr;
	fStartPut = refBuffer.Ptr;
	fEndPut = refBuffer.Ptr + refBuffer.Len;
	fBytesWritten = 0;
}

bool ResizeableStringFormatter::BufferIsFull(char* inBuffer, uint32_t inBufferLen)
{
	//allocate a buffer twice as big as the old one, and copy over the contents
	uint32_t theNewBufferSize = this->GetTotalBufferSize() * 2;
	if (theNewBufferSize == 0)
		theNewBufferSize = 64;

	char* theNewBuffer = (char *)m_sPool.Alloc(theNewBufferSize);
	::memcpy(theNewBuffer, inBuffer, inBufferLen);

	fStartPut = theNewBuffer;
	fCurrentPut = theNewBuffer + inBufferLen;
	fEndPut = theNewBuffer + theNewBufferSize;
	return true;
}



///////////////////////////////////////////////////////////////////////////////
// Class : StringParser
///////////////////////////////////////////////////////////////////////////////

uint8_t StringParser::sNonWordMask[] =
{
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //0-9
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //10-19
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //20-29
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //30-39
    1, 1, 1, 1, 1, 0, 1, 1, 1, 1, //40-49 - is a word
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //50-59
    1, 1, 1, 1, 1, 0, 0, 0, 0, 0, //60-69 //stop on every character except a letter
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //70-79
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //80-89
    0, 1, 1, 1, 1, 0, 1, 0, 0, 0, //90-99 _ is a word
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //100-109
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //110-119
    0, 0, 0, 1, 1, 1, 1, 1, 1, 1, //120-129
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //130-139
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //140-149
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //150-159
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //160-169
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //170-179
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //180-189
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //190-199
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //200-209
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //210-219
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //220-229
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //230-239
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //240-249
    1, 1, 1, 1, 1, 1             //250-255
};

uint8_t StringParser::sWordMask[] =
{
    // Inverse of the above
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //0-9
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //10-19
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //20-29
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //30-39
    0, 0, 0, 0, 0, 1, 0, 0, 0, 0, //40-49 - is a word
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //50-59
    0, 0, 0, 0, 0, 1, 1, 1, 1, 1, //60-69 //stop on every character except a letter
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //70-79
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //80-89
    1, 0, 0, 0, 0, 1, 0, 1, 1, 1, //90-99 _ is a word
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //100-109
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //110-119
    1, 1, 1, 0, 0, 0, 0, 0, 0, 0, //120-129
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //130-139
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //140-149
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //150-159
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //160-169
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //170-179
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //180-189
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //190-199
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //200-209
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //210-219
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //220-229
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //230-239
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //240-249
    0, 0, 0, 0, 0, 0             //250-255
};

uint8_t StringParser::sDigitMask[] =
{
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //0-9
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //10-19
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //20-29
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //30-39
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, //40-49 //stop on every character except a number
    1, 1, 1, 1, 1, 1, 1, 1, 0, 0, //50-59
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //60-69
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //70-79
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //80-89
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //90-99
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //100-109
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //110-119
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //120-129
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //130-139
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //140-149
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //150-159
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //160-169
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //170-179
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //180-189
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //190-199
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //200-209
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //210-219
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //220-229
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //230-239
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //240-249
    0, 0, 0, 0, 0, 0             //250-255
};

uint8_t StringParser::sEOLMask[] =
{
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //0-9
    1, 0, 0, 1, 0, 0, 0, 0, 0, 0, //10-19    //'\r' & '\n' are stop conditions
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //20-29
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //30-39
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //40-49
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //50-59
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //60-69
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //70-79
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //80-89
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //90-99
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //100-109
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //110-119
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //120-129
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //130-139
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //140-149
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //150-159
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //160-169
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //170-179
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //180-189
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //190-199
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //200-209
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //210-219
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //220-229
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //230-239
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //240-249
    0, 0, 0, 0, 0, 0             //250-255
};

uint8_t StringParser::sWhitespaceMask[] =
{
    1, 1, 1, 1, 1, 1, 1, 1, 1, 0, //0-9      // stop on '\t'
    0, 0, 0, 0, 1, 1, 1, 1, 1, 1, //10-19    // '\r', \v', '\f' & '\n'
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //20-29
    1, 1, 0, 1, 1, 1, 1, 1, 1, 1, //30-39   //  ' '
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //40-49
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //50-59
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //60-69
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //70-79
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //80-89
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //90-99
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //100-109
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //110-119
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //120-129
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //130-139
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //140-149
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //150-159
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //160-169
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //170-179
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //180-189
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //190-199
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //200-209
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //210-219
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //220-229
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //230-239
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //240-249
    1, 1, 1, 1, 1, 1             //250-255
};

uint8_t StringParser::sEOLWhitespaceMask[] =
{
    0, 0, 0, 0, 0, 0, 0, 0, 0, 1, //0-9     // \t is a stop
    1, 1, 1, 1, 0, 0, 0, 0, 0, 0, //10-19    //'\r' & '\n' are stop conditions
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //20-29
    0, 0, 1, 0, 0, 0, 0, 0, 0, 0, //30-39   ' '  is a stop
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //40-49
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //50-59
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //60-69
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //70-79
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //80-89
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //90-99
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //100-109
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //110-119
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //120-129
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //130-139
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //140-149
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //150-159
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //160-169
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //170-179
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //180-189
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //190-199
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //200-209
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //210-219
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //220-229
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //230-239
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //240-249
    0, 0, 0, 0, 0, 0             //250-255
};


uint8_t StringParser::sEOLWhitespaceQueryMask[] =
{
    0, 0, 0, 0, 0, 0, 0, 0, 0, 1, //0-9     // \t is a stop
    1, 1, 1, 1, 0, 0, 0, 0, 0, 0, //10-19    //'\r' & '\n' are stop conditions
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //20-29
    0, 0, 1, 0, 0, 0, 0, 0, 0, 0, //30-39   ' '  is a stop
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //40-49
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //50-59
    0, 0, 0, 1, 0, 0, 0, 0, 0, 0, //60-69  ? is a stop
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //70-79
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //80-89
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //90-99
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //100-109
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //110-119
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //120-129
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //130-139
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //140-149
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //150-159
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //160-169
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //170-179
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //180-189
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //190-199
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //200-209
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //210-219
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //220-229
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //230-239
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //240-249
    0, 0, 0, 0, 0, 0             //250-255
};

void StringParser::ConsumeUntil(BCStrPtrLen* outString, char inStop)
{
    if (this->ParserIsEmpty(outString))
        return;

    char *originalStartGet = fStartGet;

    while ((fStartGet < fEndGet) && (*fStartGet != inStop))
        AdvanceMark();

    if (outString != NULL)
    {
        outString->Ptr = originalStartGet;
        outString->Len = fStartGet - originalStartGet;
    }
}

void StringParser::ConsumeUntil(BCStrPtrLen* outString, uint8_t *inMask)
{
    if (this->ParserIsEmpty(outString))
        return;

    char *originalStartGet = fStartGet;

    while ((fStartGet < fEndGet) && (!inMask[(unsigned char) (*fStartGet)]))//make sure inMask is indexed with an unsigned char
        AdvanceMark();

    if (outString != NULL)
    {
        outString->Ptr = originalStartGet;
        outString->Len = fStartGet - originalStartGet;
    }
}

void StringParser::ConsumeLength(BCStrPtrLen* spl, int32_t inLength)
{
    if (this->ParserIsEmpty(spl))
        return;

    //sanity check to make sure we aren't being told to run off the end of the
    //buffer
    if ((fEndGet - fStartGet) < inLength)
        inLength = fEndGet - fStartGet;

    if (spl != NULL)
    {
        spl->Ptr = fStartGet;
        spl->Len = inLength;
    }
    if (inLength > 0)
    {
        for (short i=0; i<inLength; i++)
            AdvanceMark();
    }
    else
	{
        fStartGet += inLength;  // ***may mess up line number if we back up too much
	}
}


uint32_t StringParser::ConsumeInteger(BCStrPtrLen* outString)
{
    if (this->ParserIsEmpty(outString))
        return 0;

    uint32_t theValue = 0;
    char *originalStartGet = fStartGet;

    while ((fStartGet < fEndGet) && (*fStartGet >= '0') && (*fStartGet <= '9'))
    {
        theValue = (theValue * 10) + (*fStartGet - '0');
        AdvanceMark();
    }

    if (outString != NULL)
    {
        outString->Ptr = originalStartGet;
        outString->Len = fStartGet - originalStartGet;
    }
    return theValue;
}

float32_t StringParser::ConsumeFloat()
{
    if (this->ParserIsEmpty(NULL))
        return 0.0;

    float32_t theFloat = 0;
    while ((fStartGet < fEndGet) && (*fStartGet >= '0') && (*fStartGet <= '9'))
    {
        theFloat = (theFloat * 10) + (*fStartGet - '0');
        AdvanceMark();
    }
    if ((fStartGet < fEndGet) && (*fStartGet == '.'))
        AdvanceMark();
    float32_t multiplier = (float32_t) .1;
    while ((fStartGet < fEndGet) && (*fStartGet >= '0') && (*fStartGet <= '9'))
    {
        theFloat += (multiplier * (*fStartGet - '0'));
        multiplier *= (float32_t).1;

        AdvanceMark();
    }
    return theFloat;
}

float32_t StringParser::ConsumeNPT()
{
    if (this->ParserIsEmpty(NULL))
	    return 0.0;

    float32_t valArray[4] = {0, 0, 0, 0};
    float32_t divArray[4] = {1, 1, 1, 1};
    uint32_t valType = 0; // 0 == npt-sec, 1 == npt-hhmmss
    uint32_t index;

    for (index = 0; index < 4; index ++)
    {
        while ((fStartGet < fEndGet) && (*fStartGet >= '0') && (*fStartGet <= '9'))
        {
            valArray[index] = (valArray[index] * 10) + (*fStartGet - '0');
            divArray[index] *= 10;
            AdvanceMark();
        }

        if (fStartGet >= fEndGet || (valType == 0 && index >= 1))
            break;

        if (*fStartGet == '.' && valType == 0 && index == 0)
            ;
        else if (*fStartGet == ':' && index < 2)
            valType = 1;
        else if (*fStartGet == '.' && index == 2)
            ;
        else
            break;
        AdvanceMark();
    }

    if (valType == 0)
        return valArray[0] + (valArray[1] / divArray[1]);
    else
        return (valArray[0] * 3600) + (valArray[1] * 60) + valArray[2] + (valArray[3] / divArray[3]);
}


bool  StringParser::Expect(char stopChar)
{
    if (this->ParserIsEmpty(NULL))
        return false;

    if (fStartGet >= fEndGet)
        return false;
    if(*fStartGet != stopChar)
        return false;
    else
    {
        AdvanceMark();
        return true;
    }
}
bool StringParser::ExpectEOL()
{
    if (this->ParserIsEmpty(NULL))
        return false;

    //This function processes all legal forms of HTTP / RTSP eols.
    //They are: \r (alone), \n (alone), \r\n
    bool retVal = false;
    if ((fStartGet < fEndGet) && ((*fStartGet == '\r') || (*fStartGet == '\n')))
    {
        retVal = true;
        AdvanceMark();
        //check for a \r\n, which is the most common EOL sequence.
        if ((fStartGet < fEndGet) && ((*(fStartGet - 1) == '\r') && (*fStartGet == '\n')))
            AdvanceMark();
    }
    return retVal;
}

void StringParser::ConsumeEOL(BCStrPtrLen* outString)
{
    if (this->ParserIsEmpty(outString))
    {
        return;
    }

	//This function processes all legal forms of HTTP / RTSP eols.
	//They are: \r (alone), \n (alone), \r\n
	char *originalStartGet = fStartGet;

	if ((fStartGet < fEndGet) && ((*fStartGet == '\r') || (*fStartGet == '\n')))
	{
		AdvanceMark();
		//check for a \r\n, which is the most common EOL sequence.
		if ((fStartGet < fEndGet) && ((*(fStartGet - 1) == '\r') && (*fStartGet == '\n')))
			AdvanceMark();
	}

	if (outString != NULL)
	{
		outString->Ptr = originalStartGet;
		outString->Len = fStartGet - originalStartGet;
	}
}

void StringParser::UnQuote(BCStrPtrLen* outString)
{
    // If a string is contained within double or single quotes
    // then UnQuote() will remove them. - [sfu]

    // sanity check
    if (outString->Ptr == NULL || outString->Len < 2)
        return;

    // remove begining quote if it's there.
    if (outString->Ptr[0] == '"' || outString->Ptr[0] == '\'')
    {
        outString->Ptr++; outString->Len--;
    }
    // remove ending quote if it's there.
    if ( outString->Ptr[outString->Len-1] == '"' ||
         outString->Ptr[outString->Len-1] == '\'' )
    {
        outString->Len--;
    }
}

void StringParser::AdvanceMark()
{
     if (this->ParserIsEmpty(NULL))
        return;

   if ((*fStartGet == '\n') || ((*fStartGet == '\r') && (fStartGet[1] != '\n')))
    {
        // we are progressing beyond a line boundary (don't count \r\n twice)
        fCurLineNumber++;
    }
    fStartGet++;
}

#if STRINGPARSERTESTING
bool StringParser::Test()
{
    static char* string1 = "RTSP 200 OK\r\nContent-Type: MeowMix\r\n\t   \n3450";

    BCStrPtrLen theString(string1, strlen(string1));

    StringParser victim(&theString);

    BCStrPtrLen rtsp;
    int32_t theInt = victim.ConsumeInteger();
    if (theInt != 0)
        return false;
    victim.ConsumeWord(&rtsp);
    if ((rtsp.Len != 4) && (strncmp(rtsp.Ptr, "RTSP", 4) != 0))
        return false;

    victim.ConsumeWhitespace();
    theInt = victim.ConsumeInteger();
    if (theInt != 200)
        return false;

    return true;
}
#endif // STRINGPARSERTESTING



///////////////////////////////////////////////////////////////////////////////
// Class : StringTranslator
//       - implements StringTranslator class
///////////////////////////////////////////////////////////////////////////////


int32_t StringTranslator::DecodeURL(const char* inSrc, int32_t inSrcLen, char* ioDest, int32_t inDestLen)
{
    // return the number of chars written to ioDest
    // or OS_BadURLFormat in the case of any error.

    // inSrcLen must be > inSrcLen and the first character must be a '/'
    if ( inSrcLen <= 0  || *inSrc != '/' )
        return OS_BadURLFormat;

    //ASSERT(*inSrc == '/'); //For the purposes of '..' stripping, we assume first char is a /

    int32_t theLengthWritten = 0;
    int tempChar = 0;
    int numDotChars = 0;
    bool inQuery = false;

    while (inSrcLen > 0)
    {
        if (theLengthWritten == inDestLen)
            return OS_NotEnoughSpace;

        if (*inSrc == '?')
            inQuery = true;

        if (*inSrc == '%')
        {
            if (inSrcLen < 3)
                return OS_BadURLFormat;

            //if there is a special character in this URL, extract it
            char tempbuff[3];
            inSrc++;
            if (!isxdigit(*inSrc))
                return OS_BadURLFormat;
            tempbuff[0] = *inSrc;
            inSrc++;
            if (!isxdigit(*inSrc))
                return OS_BadURLFormat;
            tempbuff[1] = *inSrc;
            inSrc++;
            tempbuff[2] = '\0';
            sscanf(tempbuff, "%x", &tempChar);
            ASSERT(tempChar < 256);
            inSrcLen -= 3;
        }
        else if (*inSrc == '\0')
            return OS_BadURLFormat;
        else
        {
            // Any normal character just gets copied into the destination buffer
            tempChar = *inSrc;
            inSrcLen--;
            inSrc++;
        }

        if (!inQuery)       // don't do seperator parsing or .. parsing in query
        {
            //
            // If we are in a file system that uses a character besides '/' as a
            // path delimiter, we should not allow this character to appear in the URL.
            // In URLs, only '/' has control meaning.
            if ((tempChar == kPathDelimiterChar) && (kPathDelimiterChar != '/'))
                return OS_BadURLFormat;

            // Check to see if this character is a path delimiter ('/')
            // If so, we need to further check whether backup is required due to
            // dot chars that need to be stripped
            if ((tempChar == '/') && (numDotChars <= 2) && (numDotChars > 0))
            {
                ASSERT(theLengthWritten > numDotChars);
                ioDest -= (numDotChars + 1);
                theLengthWritten -= (numDotChars + 1);
            }

            *ioDest = tempChar;

            // Note that because we are doing this dotchar check here, we catch dotchars
            // even if they were encoded to begin with.

            // If this is a . , check to see if it's one of those cases where we need to track
            // how many '.'s in a row we've gotten, for stripping out later on
            if (*ioDest == '.')
            {
                ASSERT(theLengthWritten > 0);//first char is always '/', right?
                if ((numDotChars == 0) && (*(ioDest - 1) == '/'))
                    numDotChars++;
                else if ((numDotChars > 0) && (*(ioDest - 1) == '.'))
                    numDotChars++;
            }
            // If this isn't a dot char, we don't care at all, reset this value to 0.
            else
                numDotChars = 0;
        }
        else
            *ioDest = tempChar;

        theLengthWritten++;
        ioDest++;
    }

    // Before returning, "strip" any trailing "." or ".." by adjusting "theLengthWritten
    // accordingly
    if (numDotChars <= 2)
        theLengthWritten -= numDotChars;
    return theLengthWritten;
}

int32_t StringTranslator::EncodeURL(const char* inSrc, int32_t inSrcLen, char* ioDest, int32_t inDestLen)
{
    // return the number of chars written to ioDest

    int32_t theLengthWritten = 0;

    while (inSrcLen > 0)
    {
        if (theLengthWritten == inDestLen)
            return OS_NotEnoughSpace;

        //
        // Always encode 8-bit characters
        if ((unsigned char)*inSrc > 127)
        {
            if (inDestLen - theLengthWritten < 3)
                return OS_NotEnoughSpace;

            sprintf(ioDest,"%%%X",(unsigned char)*inSrc);
            ioDest += 3;
            theLengthWritten += 3;
                        inSrc++;
                        inSrcLen--;
            continue;
        }

        //
        // Only encode certain 7-bit characters
        switch (*inSrc)
        {
            // This is the URL RFC list of illegal characters.
            case (' '):
            case ('\r'):
            case ('\n'):
            case ('\t'):
            case ('<'):
            case ('>'):
            case ('#'):
            case ('%'):
            case ('{'):
            case ('}'):
            case ('|'):
            case ('\\'):
            case ('^'):
            case ('~'):
            case ('['):
            case (']'):
            case ('`'):
            case (';'):
//          case ('/'):     // this isn't really an illegal character, it's legitimatly used as a seperator in the url
            case ('?'):
            case ('@'):
            case ('='):
            case ('&'):
            case ('$'):
            case ('"'):
            {
                if ((inDestLen - theLengthWritten) < 3)
                    return OS_NotEnoughSpace;

                sprintf(ioDest,"%%%X",(int)*inSrc);
                ioDest += 3;
                theLengthWritten += 3;
                break;
            }
            default:
            {
                *ioDest = *inSrc;
                ioDest++;
                theLengthWritten++;
            }
        }

        inSrc++;
        inSrcLen--;
    }

    return theLengthWritten;
}

void StringTranslator::DecodePath(char* inSrc, uint32_t inSrcLen)
{
    for (uint32_t x = 0; x < inSrcLen; x++)
        if (inSrc[x] == '/')
            inSrc[x] = kPathDelimiterChar;
}



#if STRINGTRANSLATORTESTING
bool StringTranslator::Test()
{
    //static char* test1 = "/%5D%3f%7eAveryweird%7C/and/long/path/ya/%5d%3F%7eAveryweird%7C/and/long/p%40/ya/%5D%3F%7EAveryweird%7C/and/long/path/ya/%5D%3F%7EAveryweird%7C/and/long/path/ya/%2560%2526a%20strange%3B%23%3D%25filename"
    static char dest[1000];
    static char* test1 = "/Hello%23%20 I want%28don't%29";
    int32_t err = DecodeURL(test1, strlen(test1), dest, 1000);
    if (err != 22)
        return false;
    if (strcmp(dest, "/Hello#  I want(don't)") != 0)
        return false;
    err = DecodeURL(test1, 15, dest, 1000);
    if (err != 11)
        return false;
    if (strncmp(dest, "/Hello#  I ", 11) != 0)
        return false;
    err = DecodeURL(test1, 50, dest, 1000);
    if (err != OS_BadURLFormat)
        return false;
    if (strncmp(dest, "/Hello#  I want(don't)", 22) != 0)
    if (strcmp(dest, "/Hello#  I want(don't)") != 0)
        return false;

    err = DecodeURL(test1, strlen(test1), dest, 20);
    if (err != OS_BadURLFormat)
        return false;
    static char* test2 = "/THis%2h is a bad %28 URL!";
    err = DecodeURL(test2, strlen(test2), dest, 1000);
    if (err != OS_BadURLFormat)
        return false;

    static char* test3 = "/...whoa/../is./meeee%3e/./";
    static char* test4 = "/I want/to/sleep/..";
    static char* test5 = "/ve....rr/tire.././../..";
    static char* test6 = "/../beginnings/and/.";
    static char* test7 = "/../begin/%2e./../nin/%2e/gs/an/%2e%2e/fklf/%2e%2e./dfds%2e/%2e%2e/d/.%2e";
    err = DecodeURL(test3, strlen(test3), dest, 1000);
    err = DecodeURL(test4, strlen(test4), dest, 1000);
    err = DecodeURL(test5, strlen(test5), dest, 1000);
    err = DecodeURL(test6, strlen(test6), dest, 1000);
    err = DecodeURL(test7, strlen(test7), dest, 1000);
    return true;
}
#endif


///////////////////////////////////////////////////////////////////////////////
// Class : DateTranslator
//       - Efficient routines & data structures for converting from
//         RFC 1123 compliant date strings to local file system dates & vice versa.
///////////////////////////////////////////////////////////////////////////////

// If you assign values of 0 - 25 for all the letters, and sum up the values of
// the letters in each month, you get a table that looks like this. For instance,
// "Jul" = 9 + 20 + 11 = 40. The value of July in a C tm struct is 6, so position
// 40 = 6 in this array.

const uint32_t kMonthHashTable[] =
{
    12, 12, 12, 12, 12, 12, 12, 12, 12, 11,     // 0 - 9
    1,  12, 12, 12, 12, 12, 12, 12, 12, 12,     // 10 - 19
    12, 12, 0,  12, 12, 12, 7,  12, 12, 2,      // 20 - 29
    12, 12, 3,  12, 12, 9,  4,  8,  12, 12,     // 30 - 39
    6,  12, 5,  12, 12, 12, 12, 12, 10, 12      // 40 - 49
};
const uint32_t kMonthHashTableSize = 49;


struct tm *bc_gmtime(const time_t *timep, struct tm *result)
{
#if __MacOSX__
	return ::gmtime_r(timep, result);
#else
	struct tm *time_result = ::gmtime(timep);
	*result = *time_result;

	return result;
#endif
}

int32_t GetGMTOffset()
{
#ifdef __Win32__
	TIME_ZONE_INFORMATION tzInfo;
	DWORD theErr = ::GetTimeZoneInformation(&tzInfo);
	if (theErr == TIME_ZONE_ID_INVALID)
		return 0;

	return ((tzInfo.Bias / 60) * -1);
#else

	time_t clock = {0};
	struct tm  *tmptr= localtime(&clock);
	if (tmptr == NULL)
		return 0;

	return tmptr->tm_gmtoff / 3600;//convert seconds to  hours before or after GMT
#endif
}

int64_t  G_sMsecSince1900 = 0;
int64_t  G_sMsecSince1970 = 0;
int64_t  G_sInitialMsec = 0;
int64_t  G_sWrapTime = 0;
int64_t  G_sCompareWrap = 0;
int64_t  G_sLastTimeMilli = 0;

int64_t Milliseconds();

void TimeInitialize(void*)
{
	ASSERT (G_sInitialMsec == 0);  // do only once
	if (G_sInitialMsec != 0)
		return;
#if _MSC_VER >= 1500
	_tzset();
#else // _MSC_VER
	::tzset();
#endif // _MSC_VER

	//setup t0 value for msec since 1900

	//t.tv_sec is number of seconds since Jan 1, 1970. Convert to seconds since 1900
	int64_t the1900Sec = (int64_t) (24 * 60 * 60) * (int64_t) ((70 * 365) + 17) ;
	G_sMsecSince1900 = the1900Sec * 1000;

	G_sWrapTime = (int64_t) 0x00000001 << 32;
	G_sCompareWrap = (int64_t) 0xffffffff << 32;
	G_sLastTimeMilli = 0;

	G_sInitialMsec = Milliseconds(); //Milliseconds uses sInitialMsec so this assignment is valid only once.

	G_sMsecSince1970 = ::time(NULL);  // POSIX time always returns seconds since 1970
	G_sMsecSince1970 *= 1000;         // Convert to msec
}

static BCOnceS	G_sTimeInit = BC_ONCE_INIT;
static BCRESULT s_inittime = bc_once_do(&G_sTimeInit, TimeInitialize, NULL);

int64_t Milliseconds()
{
	int64_t curTime;
    struct timeval t;

    gettimeofday(&t, NULL);
    curTime = t.tv_sec;
    curTime *= 1000;                // sec -> msec
    curTime += t.tv_usec / 1000;    // usec -> msec

    return (curTime - G_sInitialMsec) + G_sMsecSince1970;
}

///////////////////////////////////////////////////////////////////////////////
// Class : DateTranslator
//       - Efficient routines & data structures for converting from
//         RFC 1123 compliant date strings to local file system dates & vice versa.
///////////////////////////////////////////////////////////////////////////////


int64_t  DateTranslator::ParseDate(BCStrPtrLen* inDateString)
{
    //SEE RFC 1123 for details on the date string format
    //ex: Mon, 04 Nov 1996 21:42:17 GMT

    // Parse the date buffer, filling out a tm struct
    struct tm theDateStruct;
    ::memset(&theDateStruct, 0, sizeof(theDateStruct));

    // All RFC 1123 dates are the same length.
    if (inDateString->Len != DateBuffer::kDateBufferLen)
        return 0;

    StringParser theDateParser(inDateString);

    // the day of the week is redundant... we can skip it!
    theDateParser.ConsumeLength(NULL, 5);

    // We are at the date now.
    theDateStruct.tm_mday = theDateParser.ConsumeInteger(NULL);
    theDateParser.ConsumeWhitespace();

    // We are at the month now. Use our hand-crafted perfect hash table
    // to get the right value to place in the tm struct
    if (theDateParser.GetDataRemaining() < 4)
        return 0;

    uint32_t theIndex =   ConvertCharToMonthTableIndex(theDateParser.GetCurrentPosition()[0]) +
                        ConvertCharToMonthTableIndex(theDateParser.GetCurrentPosition()[1]) +
                        ConvertCharToMonthTableIndex(theDateParser.GetCurrentPosition()[2]);

    if (theIndex > kMonthHashTableSize)
        return 0;

    theDateStruct.tm_mon = kMonthHashTable[theIndex];

    // If the month is illegal, return an error
    if (theDateStruct.tm_mon >= 12)
        return 0;

    // Skip over the date
    theDateParser.ConsumeLength(NULL, 4);

    // Grab the year (years since 1900 is what the tm struct wants)
    theDateStruct.tm_year = theDateParser.ConsumeInteger(NULL) - 1900;
    theDateParser.ConsumeWhitespace();

    // Now just grab hour, minute, second
    theDateStruct.tm_hour = theDateParser.ConsumeInteger(NULL);
    theDateStruct.tm_hour += GetGMTOffset();

    theDateParser.ConsumeLength(NULL, 1); //skip over ':'

    theDateStruct.tm_min = theDateParser.ConsumeInteger(NULL);
    theDateParser.ConsumeLength(NULL, 1); //skip over ':'

    theDateStruct.tm_sec = theDateParser.ConsumeInteger(NULL);

    // Ok, we've filled out the tm struct completely, now convert it to a time_t
    time_t theTime = ::mktime(&theDateStruct);
    return (int64_t)theTime * 1000; // convert to a time value in our timebase.
}

void DateTranslator::UpdateDateBuffer(DateBuffer* inDateBuffer, const int64_t& inDate, time_t gmtoffset)
{
    if (inDateBuffer == NULL)
        return;

    struct tm* gmt = NULL;
    struct tm  timeResult;

    if (inDate == 0)
    {
        time_t calendarTime = ::time(NULL) + gmtoffset;
        gmt = bc_gmtime(&calendarTime, &timeResult);
    }
    else
    {
        time_t convertedTime = (time_t)(inDate / (int64_t)1000) + gmtoffset ; // Convert from msec to sec
        gmt = bc_gmtime(&convertedTime, &timeResult);
    }

    ASSERT(gmt != NULL); //is it safe to assert this?
    size_t size  = 0;
    if (0 == gmtoffset)
        size = strftime(   inDateBuffer->fDateBuffer, sizeof(inDateBuffer->fDateBuffer),
                            "%a, %d %b %Y %H:%M:%S GMT", gmt);

    ASSERT(size == DateBuffer::kDateBufferLen);
}

void DateBuffer::InexactUpdate()
{
    int64_t theCurTime = Milliseconds();
    if ((fLastDateUpdate == 0) || ((fLastDateUpdate + kUpdateInterval) < theCurTime))
    {
        fLastDateUpdate = theCurTime;
        this->Update(0);
    }
}


///////////////////////////////////////////////////////////////////////////////
// End of namespace : HTTP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace HTTP

///////////////////////////////////////////////////////////////////////////////
// End of file : StringUtils.cpp
///////////////////////////////////////////////////////////////////////////////
