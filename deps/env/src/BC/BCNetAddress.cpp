

#include <BC/BCNetAddress.h>

#include <stddef.h>
#include <stdio.h>
#if defined(__WIN32__) || defined(_WIN32)
#include <WinSock2.h>
#include <WS2tcpip.h>
#define USE_GETHOSTBYNAME 1 /*because at least some Windows don't have getaddrinfo()*/
#else
#ifndef INADDR_NONE
#define INADDR_NONE 0xFFFFFFFF
#endif
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#endif

unsigned our_inet_addr(char const*cp)
{
	return inet_addr(cp);
}

////////// BCNetAddress //////////

BCNetAddress::BCNetAddress(uint8_t const* data, unsigned length) {
  assign(data, length);
}

BCNetAddress::BCNetAddress(unsigned length) {
  fData = new uint8_t[length];
  if (fData == NULL) {
    fLength = 0;
    return;
  }

  for (unsigned i = 0; i < length; ++i)	fData[i] = 0;
  fLength = length;
}

BCNetAddress::BCNetAddress(BCNetAddress const& orig) {
  assign(orig.data(), orig.length());
}

BCNetAddress& BCNetAddress::operator=(BCNetAddress const& rightSide) {
  if (&rightSide != this) {
    clean();
    assign(rightSide.data(), rightSide.length());
  }
  return *this;
}

BCNetAddress::~BCNetAddress() {
  clean();
}

void BCNetAddress::assign(uint8_t const* data, unsigned length) {
  fData = new uint8_t[length];
  if (fData == NULL) {
    fLength = 0;
    return;
  }

  for (unsigned i = 0; i < length; ++i)	fData[i] = data[i];
  fLength = length;
}

void BCNetAddress::clean() {
  delete[] fData; fData = NULL;
  fLength = 0;
}


////////// BCNetAddressList //////////

BCNetAddressList::BCNetAddressList(char const* hostname)
  : fNumAddresses(0), fAddressArray(NULL) {
  // First, check whether "hostname" is an IP address string:
  netAddressBits addr = our_inet_addr((char*)hostname);
  if (addr != INADDR_NONE) {
    // Yes, it was an IP address string.  Return a 1-element list with this address:
    fNumAddresses = 1;
    fAddressArray = new BCNetAddress*[fNumAddresses];
    if (fAddressArray == NULL) return;

    fAddressArray[0] = new BCNetAddress((uint8_t*)&addr, sizeof (netAddressBits));
    return;
  }
    
  // "hostname" is not an IP address string; try resolving it as a real host name instead:
#if defined(USE_GETHOSTBYNAME) || defined(VXWORKS)
  struct hostent* host;
#if defined(VXWORKS)
  char hostentBuf[512];

  host = (struct hostent*)resolvGetHostByName((char*)hostname, (char*)&hostentBuf, sizeof hostentBuf);
#else
  host = gethostbyname((char*)hostname);
#endif
  if (host == NULL || host->h_length != 4 || host->h_addr_list == NULL) return; // no luck

  uint8_t const** const hAddrPtr = (uint8_t const**)host->h_addr_list;
  // First, count the number of addresses:
  uint8_t const** hAddrPtr1 = hAddrPtr;
  while (*hAddrPtr1 != NULL) {
    ++fNumAddresses;
    ++hAddrPtr1;
  }

  // Next, set up the list:
  fAddressArray = new BCNetAddress*[fNumAddresses];
  if (fAddressArray == NULL) return;

  for (unsigned i = 0; i < fNumAddresses; ++i) {
    fAddressArray[i] = new BCNetAddress(hAddrPtr[i], host->h_length);
  }
#else
  // Use "getaddrinfo()" (rather than the older, deprecated "gethostbyname()"):
  struct addrinfo addrinfoHints;
  memset(&addrinfoHints, 0, sizeof addrinfoHints);
  addrinfoHints.ai_family = AF_INET; // For now, we're interested in IPv4 addresses only
  struct addrinfo* addrinfoResultPtr = NULL;
  int result = getaddrinfo(hostname, NULL, &addrinfoHints, &addrinfoResultPtr);
  if (result != 0 || addrinfoResultPtr == NULL) return; // no luck

  // First, count the number of addresses:
  const struct addrinfo* p = addrinfoResultPtr;
  while (p != NULL) {
    if (p->ai_addrlen < 4) continue; // sanity check: skip over addresses that are too small
    ++fNumAddresses;
    p = p->ai_next;
  }

  // Next, set up the list:
  fAddressArray = new BCNetAddress*[fNumAddresses];
  if (fAddressArray == NULL) return;

  unsigned i = 0;
  p = addrinfoResultPtr;
  while (p != NULL) {
    if (p->ai_addrlen < 4) continue;
    fAddressArray[i++] = new BCNetAddress((uint8_t const*)&(((struct sockaddr_in*)p->ai_addr)->sin_addr.s_addr), 4);
    p = p->ai_next;
  }

  // Finally, free the data that we had allocated by calling "getaddrinfo()":
  freeaddrinfo(addrinfoResultPtr);
#endif
}

BCNetAddressList::BCNetAddressList(BCNetAddressList const& orig) {
  assign(orig.numAddresses(), orig.fAddressArray);
}

BCNetAddressList& BCNetAddressList::operator=(BCNetAddressList const& rightSide) {
  if (&rightSide != this) {
    clean();
    assign(rightSide.numAddresses(), rightSide.fAddressArray);
  }
  return *this;
}

BCNetAddressList::~BCNetAddressList() {
  clean();
}

void BCNetAddressList::assign(unsigned numAddresses, BCNetAddress** addressArray) {
  fAddressArray = new BCNetAddress*[numAddresses];
  if (fAddressArray == NULL) {
    fNumAddresses = 0;
    return;
  }

  for (unsigned i = 0; i < numAddresses; ++i) {
    fAddressArray[i] = new BCNetAddress(*addressArray[i]);
  }
  fNumAddresses = numAddresses;
}

void BCNetAddressList::clean() {
  while (fNumAddresses-- > 0) {
    delete fAddressArray[fNumAddresses];
  }
  delete[] fAddressArray; fAddressArray = NULL;
}

BCNetAddress const* BCNetAddressList::firstAddress() const {
  if (fNumAddresses == 0) return NULL;

  return fAddressArray[0];
}

////////// BCNetAddressList::Iterator //////////
BCNetAddressList::Iterator::Iterator(BCNetAddressList const& addressList)
  : fAddressList(addressList), fNextIndex(0) {}

BCNetAddress const* BCNetAddressList::Iterator::nextAddress() {
  if (fNextIndex >= fAddressList.numAddresses()) return NULL; // no more
  return fAddressList.fAddressArray[fNextIndex++];
}

