
#ifndef BCTASKEVENT_INCLUDED__
#define BCTASKEVENT_INCLUDED__

#include "BC/Exports.h"
#include "BC/BCNodeList.h"
#include "BC/BCMagic.h"

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// Macros :
///////////////////////////////////////////////////////////////////////////////

/*%
 * An event class is an unsigned 16 bit number.  Each class may contain up
 * to 65536 events.  An event type is formed by adding the event number
 * within the class to the class number.
 *
 */

#define BC_EVENTCLASS(eclass)		((eclass) << 16)

/*@{*/
/*!
 * Classes < 1024 are reserved for BC use.
 * Event classes >= 1024 and <= 65535 are reserved for application use.
 */

#define	BC_EVENTCLASS_TASK			BC_EVENTCLASS(0)
#define	BC_EVENTCLASS_TIMER			BC_EVENTCLASS(1)
#define	BC_EVENTCLASS_SOCKET		BC_EVENTCLASS(2)
#define	BC_EVENTCLASS_FILE			BC_EVENTCLASS(3)
#define	BC_EVENTCLASS_DNS			BC_EVENTCLASS(4)
#define	BC_EVENTCLASS_APP			BC_EVENTCLASS(5)
#define	BC_EVENTCLASS_OMAPI			BC_EVENTCLASS(6)
#define	BC_EVENTCLASS_RATELIMITER	BC_EVENTCLASS(7)
#define	BC_EVENTCLASS_ISCCC			BC_EVENTCLASS(8)
/*@}*/

///////////////////////////////////////////////////////////////////////////////
// typedefs
///////////////////////////////////////////////////////////////////////////////


#define BC_TASKEVENT_MAGIC		BC_MAGIC('t', 's', 'e', 'v')

class BCTask;
class BCTaskEvent;

typedef void (*LPFN_BCTaskAction)(BCTask *, BCTaskEvent *);
typedef void (*LPFN_BCTaskEventDtor)(BCTaskEvent *);

typedef uint32_t					BCEventType;


///////////////////////////////////////////////////////////////////////////////
// class : BCTaskEvent - Note : Create instance with 'new' operator
///////////////////////////////////////////////////////////////////////////////

class BC_API BCTaskEvent
	: public BCNodeList::Node
	, public BCMagic

{
	template<typename>	friend class BCListT;
public:
	BCTaskEvent();
	BCTaskEvent(
		void *sender,
		BCEventType type,
		LPFN_BCTaskAction action,
		const void *arg);
	virtual ~BCTaskEvent();

	virtual void	Destroy();

public:
	uint32_t					ev_attributes;
	void					*	ev_tag;
	BCEventType					ev_type;
	LPFN_BCTaskAction			ev_action;
	void					*	ev_arg;
	void					*	ev_sender;
protected:
private:
	DECLARE_NO_COPY_CLASS(BCTaskEvent);
};

typedef TNodeList<BCTaskEvent>		BCTaskEventList;

/*%
 * Attributes matching a mask of 0x000000ff are reserved for the task library's
 * definition.  Attributes of 0xffffff00 may be used by the application
 * or non-BC libraries.
 */
#define BC_EVENTATTR_NOPURGE		0x00000001

/*%
 * The BC_EVENTATTR_CANCELED attribute is intended to indicate
 * that an event is delivered as a result of a canceled operation
 * rather than successful completion, by mutual agreement
 * between the sender and receiver.  It is not set or used by
 * the task system.
 */
#define BC_EVENTATTR_CANCELED		0x00000002

///////////////////////////////////////////////////////////////////////////////
// End of namespace :
///////////////////////////////////////////////////////////////////////////////

};

#endif

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
