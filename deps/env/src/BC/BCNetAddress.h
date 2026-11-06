
#ifndef BCNETADDRESS_H_INCLUDED__
#define BCNETADDRESS_H_INCLUDED__

#include <stdint.h>
#include <BC/Exports.h>

#ifdef HAVE_SOCKADDR_LEN
#define SET_SOCKADDR_SIN_LEN(var) var.sin_len = sizeof var
#else
#define SET_SOCKADDR_SIN_LEN(var)
#endif

#define MAKE_SOCKADDR_IN(var,adr,prt) /*adr,prt must be in network order*/\
    struct sockaddr_in var;\
    var.sin_family = AF_INET;\
    var.sin_addr.s_addr = (adr);\
    var.sin_port = (prt);\
    SET_SOCKADDR_SIN_LEN(var);

// Definition of a type representing a low-level network address.
// At present, this is 32-bits, for IPv4.  Later, generalize it,
// to allow for IPv6.
typedef uint32_t netAddressBits;

class BC_API BCNetAddress {
public:
  BCNetAddress(uint8_t const* data,
	     unsigned length = 4 /* default: 32 bits */);
  BCNetAddress(unsigned length = 4); // sets address data to all-zeros
  BCNetAddress(BCNetAddress const& orig);
  BCNetAddress& operator=(BCNetAddress const& rightSide);
  virtual ~BCNetAddress();
  
  unsigned length() const { return fLength; }
  uint8_t const* data() const // always in network byte order
  { return fData; }
  
private:
  void assign(uint8_t const* data, unsigned length);
  void clean();
  
  unsigned fLength;
  uint8_t* fData;
};

class BC_API BCNetAddressList {
public:
  BCNetAddressList(char const* hostname);
  BCNetAddressList(BCNetAddressList const& orig);
  BCNetAddressList& operator=(BCNetAddressList const& rightSide);
  virtual ~BCNetAddressList();
  
  unsigned numAddresses() const { return fNumAddresses; }
  
  BCNetAddress const* firstAddress() const;
  
  // Used to iterate through the addresses in a list:
  class Iterator {
  public:
    Iterator(BCNetAddressList const& addressList);
    BCNetAddress const* nextAddress(); // NULL iff none
  private:
    BCNetAddressList const& fAddressList;
    unsigned fNextIndex;
  };
  
private:
  void assign(netAddressBits numAddresses, BCNetAddress** addressArray);
  void clean();
  
  friend class Iterator;
  unsigned fNumAddresses;
  BCNetAddress** fAddressArray;
};

typedef uint16_t portNumBits;

class BC_API Port {
public:
  Port(portNumBits num /* in host byte order */);
  
  portNumBits num() const { return fPortNum; } // in network byte order
  
private:
  portNumBits fPortNum; // stored in network byte order
#ifdef IRIX
  portNumBits filler; // hack to overcome a bug in IRIX C++ compiler
#endif
};

#endif // BCNETADDRESS_H_INCLUDED__
