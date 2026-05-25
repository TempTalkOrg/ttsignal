///////////////////////////////////////////////////////////////////////////////
// file : NetStatus.cpp
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////


#include <BC/BCTime.h>
#include <RTMP/NetStatus.h>

using namespace BC;

/*


info	property
info:Object
Language Version: 	ActionScript 3.0
Runtime Versions: 	AIR 1.0, Flash Player 9, Flash Lite 4
An object with properties that describe the object's status or error condition.

The information object could have a code property containing a string that represents a
specific event or a level property containing a string that is either "status" or "error".

The information object could also be something different. The code and level properties
might not work for some implementations and some servers might send different objects.

P2P connections send messages to a NetConnection with a stream parameter in the information
object that indicates which NetStream the message pertains to.

For example, Flex Data Services sends Message objects that cause coercion errors if you try
to access the code or level property.

The following table describes the possible string values of the code and level properties.

Code property	                      Level property	Meaning
"NetConnection.Call.BadVersion"	         "error"	Packet encoded in an unidentified format.
"NetConnection.Call.Failed"	             "error"	The NetConnection.call() method was not
able to invoke the server-side method or command.
"NetConnection.Call.Prohibited"	         "error"	An Action Message Format (AMF) operation
is prevented for security reasons. Either the AMF URL is not in the same domain as the file
containing the code calling the NetConnection.call() method, or the AMF server does not have
a policy file that trusts the domain of the the file containing the code calling the
NetConnection.call() method.
"NetConnection.Connect.AppShutdown"	     "error"	The server-side application is shutting down.
"NetConnection.Connect.Closed"	         "status"	The connection was closed successfully.
"NetConnection.Connect.Failed"	         "error"	The connection attempt failed.
"NetConnection.Connect.IdleTimeout"	     "status"	Flash Media Server disconnected the client
because the client was idle longer than the configured value for <MaxIdleTime>. On Flash Media
Server, <AutoCloseIdleClients> is disabled by default. When enabled, the default timeout value
is 3600 seconds (1 hour). For more information, see Close idle connections.
"NetConnection.Connect.InvalidApp"	     "error"	The application name specified in the call
to NetConnection.connect() is invalid.
"NetConnection.Connect.NetworkChange"	 "status"	Flash Player has detected a network change,
for example, a dropped wireless connection, a successful wireless connection,or a network cable
loss. Use this event to check for a network interface change. Don't use this event to implement
your NetConnection reconnect logic. Use "NetConnection.Connect.Closed" to implement your
NetConnection reconnect logic.
"NetConnection.Connect.Rejected"	     "error"	The connection attempt did not have permission
to access the application.
"NetConnection.Connect.Success"	         "status"	The connection attempt succeeded.
"NetGroup.Connect.Failed"	             "error"	The NetGroup connection attempt failed. The
info.group property indicates which NetGroup failed.
"NetGroup.Connect.Rejected"	             "error"	The NetGroup is not authorized to function.
The info.group property indicates which NetGroup was denied.
"NetGroup.Connect.Succcess"	             "status"	The NetGroup is successfully constructed and
authorized to function. The info.group property indicates which NetGroup has succeeded.
"NetGroup.LocalCoverage.Notify"	         "status"	Sent when a portion of the group address
space for which this node is responsible changes.
"NetGroup.MulticastStream.PublishNotify"	"status"	Sent when a new named stream is detected
in NetGroup's Group. The info.name:String property is the name of the detected stream.
"NetGroup.MulticastStream.UnpublishNotify"	"status"	Sent when a named stream is no longer
available in the Group. The info.name:String property is name of the stream which has disappeared.
"NetGroup.Neighbor.Connect"	             "status"	Sent when a neighbor connects to this node.
The info.neighbor:String property is the group address of the neighbor. The info.peerID:String
property is the peer ID of the neighbor.
"NetGroup.Neighbor.Disconnect"	         "status"	Sent when a neighbor disconnects from this
node. The info.neighbor:String property is the group address of the neighbor. The info.peerID:String
property is the peer ID of the neighbor.
"NetGroup.Posting.Notify"	             "status"	Sent when a new Group Posting is received.
The info.message:Object property is the message. The info.messageID:String property is this
message's messageID.
"NetGroup.Replication.Fetch.Failed"	     "status"	Sent when a fetch request for an object
(previously announced with NetGroup.Replication.Fetch.SendNotify) fails or is denied. A new
attempt for the object will be made if it is still wanted. The info.index:Number property is
the index of the object that had been requested.
"NetGroup.Replication.Fetch.Result"	     "status"	Sent when a fetch request was satisfied
by a neighbor. The info.index:Number property is the object index of this result. The
info.object:Object property is the value of this object. This index will automatically be
removed from the Want set. If the object is invalid, this index can be re-added to the Want
set with NetGroup.addWantObjects().
"NetGroup.Replication.Fetch.SendNotify"	 "status"	Sent when the Object Replication system
is about to send a request for an object to a neighbor.The info.index:Number property is the
index of the object that is being requested.
"NetGroup.Replication.Request"	         "status"	Sent when a neighbor has requested an object
that this node has announced with NetGroup.addHaveObjects(). This request must eventually be
answered with either NetGroup.writeRequestedObject() or NetGroup.denyRequestedObject(). Note that
the answer may be asynchronous. The info.index:Number property is the index of the object that
has been requested. The info.requestID:int property is the ID of this request, to be used by
NetGroup.writeRequestedObject() or NetGroup.denyRequestedObject().
"NetGroup.SendTo.Notify"	             "status"	Sent when a message directed to this node
is received. The info.message:Object property is the message. The info.from:String property is
the groupAddress from which the message was received. The info.fromLocal:Boolean property is
TRUE if the message was sent by this node (meaning the local node is the nearest to the
destination group address), and FALSE if the message was received from a different node.
To implement recursive routing, the message must be resent with NetGroup.sendToNearest() if
info.fromLocal is FALSE.
"NetStream.Buffer.Empty"	             "status"	Flash Player is not receiving data quickly
enough to fill the buffer. Data flow is interrupted until the buffer refills, at which time a
NetStream.Buffer.Full message is sent and the stream begins playing again.
"NetStream.Buffer.Flush"	             "status"	Data has finished streaming, and the
remaining buffer is emptied.
"NetStream.Buffer.Full"	                 "status"	The buffer is full and the stream begins playing.
"NetStream.Connect.Closed"	             "status"	The P2P connection was closed successfully.
The info.stream property indicates which stream has closed.
"NetStream.Connect.Failed"	             "error"	The P2P connection attempt failed. The
info.stream property indicates which stream has failed.
"NetStream.Connect.Rejected"	         "error"	The P2P connection attempt did not have
permission to access the other peer. The info.stream property indicates which stream was rejected.
"NetStream.Connect.Success"	             "status"	The P2P connection attempt succeeded. The
info.stream property indicates which stream has succeeded.
"NetStream.DRM.UpdateNeeded"	         "status"	A NetStream object is attempting to play
protected content, but the required Flash Access module is either not present, not permitted
by the effective content policy, or not compatible with the current player. To update the module
or player, use the update() method of flash.system.SystemUpdater.
"NetStream.Failed"	                     "error"	(Flash Media Server) An error has occurred
for a reason other than those listed in other event codes.
"NetStream.MulticastStream.Reset"	     "status"	A multicast subscription has changed focus
to a different stream published with the same name in the same group. Local overrides of
multicast stream parameters are lost. Reapply the local overrides or the new stream's default
parameters will be used.
"NetStream.Pause.Notify"	             "status"	The stream is paused.
"NetStream.Play.Failed"	                 "error"	An error has occurred in playback for a
reason other than those listed elsewhere in this table, such as the subscriber not having read
access.
"NetStream.Play.FileStructureInvalid"	 "error"	(AIR and Flash Player 9.0.115.0) The
application detects an invalid file structure and will not try to play this type of file.
"NetStream.Play.InsufficientBW"	         "warning"	(Flash Media Server) The client does not
have sufficient bandwidth to play the data at normal speed.
"NetStream.Play.NoSupportedTrackFound"	 "error"	(AIR and Flash Player 9.0.115.0) The
application does not detect any supported tracks (video, audio or data) and will not try to
play the file.
"NetStream.Play.PublishNotify"	         "status"	The initial publish to a stream is sent
to all subscribers.
"NetStream.Play.Reset"	                 "status"	Caused by a play list reset.
"NetStream.Play.Start"	                 "status"	Playback has started.
"NetStream.Play.Stop"	                 "status"	Playback has stopped.
"NetStream.Play.StreamNotFound"	         "error"	The file passed to the NetStream.play()
method can't be found.
"NetStream.Play.Transition"	             "status"	(Flash Media Server 3.5) The server
received the command to transition to another stream as a result of bitrate stream switching.
This code indicates a success status event for the NetStream.play2() call to initiate a stream
switch. If the switch does not succeed, the server sends a NetStream.Play.Failed event instead.
When the stream switch occurs, an onPlayStatus event with a code of
"NetStream.Play.TransitionComplete" is dispatched. For Flash Player 10 and later.
"NetStream.Play.UnpublishNotify"	     "status"	An unpublish from a stream is sent to
all subscribers.
"NetStream.Publish.BadName"	             "error"	Attempt to publish a stream which is
already being published by someone else.
"NetStream.Publish.Idle"	             "status"	The publisher of the stream is idle
and not transmitting data.
"NetStream.Publish.Start"	             "status"	Publish was successful.
"NetStream.Record.AlreadyExists"	     "status"	The stream being recorded maps to a file
that is already being recorded to by another stream. This can happen due to misconfigured
virtual directories.
"NetStream.Record.Failed"	             "error"	An attempt to record a stream failed.
"NetStream.Record.NoAccess"	             "error"	Attempt to record a stream that is still
playing or the client has no access right.
"NetStream.Record.Start"	             "status"	Recording has started.
"NetStream.Record.Stop"	                 "status"	Recording stopped.
"NetStream.Seek.Failed"	                 "error"	The seek fails, which happens if the
stream is not seekable.
"NetStream.Seek.InvalidTime"	         "error"	For video downloaded progressively,
the user has tried to seek or play past the end of the video data that has downloaded thus
far, or past the end of the video once the entire file has downloaded. The info.details
property of the event object contains a time code that indicates the last valid position
to which the user can seek.
"NetStream.Seek.Notify"	                 "status"	The seek operation is complete. Sent
when NetStream.seek() is called on a stream in AS3 NetStream Data Generation Mode. The info
object is extended to include info.seekPoint which is the same value passed to NetStream.seek().
"NetStream.Step.Notify"	                 "status"	The step operation is complete.
"NetStream.Unpause.Notify"	             "status"	The stream is resumed.
"NetStream.Unpublish.Success"	         "status"	The unpublish operation was successfuul.
"SharedObject.BadPersistence"	         "error"	A request was made for a shared object with
persistence flags, but the request cannot be granted because the object has already been created
with different flags.
"SharedObject.Flush.Failed"	             "error"	The "pending" status is resolved, but the
SharedObject.flush() failed.
"SharedObject.Flush.Success"	         "status"	The "pending" status is resolved and the
SharedObject.flush() call succeeded.
"SharedObject.UriMismatch"	             "error"	An attempt was made to connect to a
NetConnection object that has a different URI (URL) than the shared object.


*/

///////////////////////////////////////////////////////////////////////////////
// Namespace : StatusCode
///////////////////////////////////////////////////////////////////////////////

namespace StatusCode
{
const char *S_NS_PLAYSTART					= "NetStream.Play.Start";
const char *S_NS_PLAYSTOP					= "NetStream.Play.Stop";
const char *S_NS_PLAYRESET					= "NetStream.Play.Reset";
const char *S_NS_PLAYPUBLISHNOTIFY			= "NetStream.Play.PublishNotify";
const char *S_NS_PLAYUNPUBLISHNOTIFY		= "NetStream.Play.UnpublishNotify";
const char *S_NS_PLAYSWITCH					= "NetStream.Play.Switch";
const char *S_NS_PLAYCOMPLETE				= "NetStream.Play.Complete";
const char *S_NS_PLAYTRANSITIONCOMPLETE		= "NetStream.Play.TransitionComplete";
const char *S_NS_PUBLISHSTART				= "NetStream.Publish.Start";
const char *S_NS_PUBLISHIDLE				= "NetStream.Publish.Idle";
const char *S_NS_UNPUBLISHSUCCESS			= "NetStream.Unpublish.Success";
const char *S_NS_SEEKNOTIFY					= "NetStream.Seek.Notify";
const char *S_NS_RECORDSTART				= "NetStream.Record.Start";
const char *S_NS_RECORDSTOP					= "NetStream.Record.Stop";
const char *S_NS_PAUSENOTIFY				= "NetStream.Pause.Notify";
const char *S_NS_UNPAUSENOTIFY				= "NetStream.Unpause.Notify";
const char *S_SO_FLUSHSUCCESS				= "SharedObject.Flush.Success";
const char *S_NS_BUFFEREMPTY				= "NetStream.Buffer.Empty";
const char *S_NS_BUFFERFULL					= "NetStream.Buffer.Full";
const char *S_NS_BUFFERFLUSH				= "NetStream.Buffer.Flush";
const char *S_NS_CONNECTSUCCESS				= "NetStream.Connect.Success"; // P2P
const char *S_NS_CONNECTCLOSED				= "NetStream.Connect.Closed"; // P2P
const char *E_NC_CONNECTFAILED				= "NetConnection.Connect.Failed";
const char *E_NC_CONNECTREJECTED			= "NetConnection.Connect.Rejected";
const char *E_NC_CONNECTAPPSHUTDOWN			= "NetConnection.Connect.AppShutdown";
const char *E_NC_CONNECTINVALIDAPP			= "NetConnection.Connect.InvalidApp";
const char *E_NC_CALLBADVERSION				= "NetConnection.Call.BadVersion";
const char *E_NC_CALLFAILED					= "NetConnection.Call.Failed";
const char *E_NC_CALLPROHIBITED				= "NetConnection.Call.Prohibited";
const char *E_NS_CREATESTREAMFAILED			= "NetStream.CreateStream.Failed";
const char *E_NS_PLAYFAILED					= "NetStream.Play.Failed";
const char *E_NS_PLAYSTREAMNOTFOUND			= "NetStream.Play.StreamNotFound";
const char *E_NS_PLAYINSUFFICIENTBW			= "NetStream.Play.InsufficientBW";
const char *E_NS_PLAYFILESTRUCTUREINVALID	= "NetStream.Play.FileStructureInvalid";
const char *E_NS_PLAYNOSUPPORTEDTRACKFOUND	= "NetStream.Play.NoSupportedTrackFound";
const char *E_NS_PUBLISHBADNAME				= "NetStream.Publish.BadName";
const char *E_NS_SEEKFAILED					= "NetStream.Seek.Failed";
const char *E_NS_SEEKINVALIDTIME			= "NetStream.Seek.InvalidTime";
const char *E_NS_RECORDNOACCESS				= "NetStream.Record.NoAccess";
const char *E_NS_RECORDFAILED				= "NetStream.Record.Failed";
const char *E_NS_PAUSEFAILED				= "NetStream.Pause.Failed";
const char *E_NS_CONNECTFAILED				= "NetStream.Connect.Failed"; // P2P
const char *E_NS_CONNECTREJECTED			= "NetStream.Connect.Rejected"; // P2P
const char *E_SO_FLUSHFAILED				= "SharedObject.Flush.Failed";
const char *E_SO_BADPERSISTENCE				= "SharedObject.BadPersistence";
const char *E_SO_URIMISMATCH				= "SharedObject.UriMismatch";
const char *E_SO_NOREADACCESS				= "SharedObject.NoReadAccess";
const char *E_SO_NOWRITEACCESS				= "SharedObject.NoWriteAccess";
const char *E_SO_OBJECTCREATIONFAILED		= "SharedObject.ObjectCreationFailed";
const char *R_NC_CONNECTSUCCESS				= "NetConnection.Connect.Success";
const char *R_NC_CONNECTCLOSED				= "NetConnection.Connect.Closed";
const char *R_NS_CREATESTREAMSUCCESS		= "NetStream.CreateStream.Success";
const char *R_NS_RELEASESTREAMSUCCESS		= "NetStream.ReleaseStream.Success";
const char *E_NG_CONNECTFAILED              = "NetGroup.Connect.Failed";
const char *E_NG_CONNECTREJECTED            = "NetGroup.Connect.Rejected";
const char *R_NG_CONNECTSUCCESS             = "NetGroup.Connect.Success";
const char *R_NG_LOCALCOVERAGENOTIFY        = "NetGroup.LocalCoverage.Notify";
const char *R_NG_MULTICASTSTREAMPUBLISHNOTIFY   = "NetGroup.MulticastStream.PublishNotify";
const char *R_NG_MULTICASTSTREAMUNPUBLISHNOTIFY = "NetGroup.MulticastStream.UnpublishNotify";
const char *R_NG_NEIGHBORCONNECT            = "NetGroup.Neighbor.Connect";
const char *R_NG_NEIGHBORDISCONNECT         = "NetGroup.Neighbor.Disconnect";
const char *R_NG_POSTINGNOTIFY              = "NetGroup.Posting.Notify";
const char *R_NG_REPLICATIONFETCHFAILED     = "NetGroup.Replication.Fetch.Failed";
const char *R_NG_REPLICATIONFETCHRESULT     = "NetGroup.Replication.Fetch.Result";
const char *R_NG_REPLICATIONFETCHSENDNOTIFY = "NetGroup.Replication.Fetch.SendNotify";
const char *R_NG_REPLICATIONREQUEST         = "NetGroup.Replication.Request";
const char *R_NG_SENDTONOTIFY               = "NetGroup.SendTo.Notify";
const char *R_NC_CALLSUCCESS				= "NetConnection.Call.Success";

///////////////////////////////////////////////////////////////////////////////
// End of namespace : StatusCode
///////////////////////////////////////////////////////////////////////////////
}

//////////////////////////////////////////////////////////////////////////
/// namespace RTMP
//////////////////////////////////////////////////////////////////////////

namespace RTMP
{

static BCPString getVersion() 
{
	//BCFVar *pValue;
	//BCPString strVersion;
	//BCOptions *pOptions = &BCGetKeySystem();

	//pValue = pOptions->GetOption(RTMP_SERVER_BASEPRODUCT);
	//if (!IS_BCF_NULL(pValue) && !IS_BCF_STRING(pValue)) {
	//	BC_SAFE_DELETE_PTR(pValue);
	//} else {
	//	if (IS_BCF_STRING(pValue)) {
	//		strVersion = GET_BCF_STRING(pValue).c_str();
	//	} else {
	//		strVersion = NULLPSTRING;
	//	}
	//	BC_SAFE_DELETE_PTR(pValue);
	//}

	BCPString strVersion("0.1.0");
	return strVersion;
}

//////////////////////////////////////////////////////////////////////////
/// class MRConnect
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(MRConnect, 8);

MRConnect::MRConnect(
	uint32_t eStatus,
	uint32_t nChannelId,
	uint32_t eEncoding /*= ObjectEncoding::AMF0*/)
		: MCommand(nChannelId, 0, 0)
		, m_eStatusType(eStatus)
{
	BCPString strVersion = getVersion();
	BCPString strPruduct("FMS");
	switch(m_eStatusType)
	{
	case CONNECT_SUCCESS:
		{
			MCommand::SetCmdName(MCommand::_result);
			MCommand::AddStringOption(s_szLevel, s_szStatus);
			MCommand::AddStringOption(s_szCode, StatusCode::R_NC_CONNECTSUCCESS);
			MCommand::AddStringOption(s_szDescription, "Connection succeeded.");
			MCommand::AddDoubleOption("objectEncoding", eEncoding);

			uint32_t tmNow;
			bc_stdtime_get(&tmNow);
			MCommand::AddDoubleOption("time", tmNow);
			AMFVarPtr pData(new AMF0ECMAArray());
			if (pData)
			{
				AMFCast<AMF0ECMAArray>(pData)->PutString("version", strVersion);
				MCommand::AddVarOption("data", pData);
			}
		}
		break;
	case CONNECT_CLOSED:
		MCommand::SetCmdName(MCommand::_result);
		MCommand::AddStringOption(s_szLevel, s_szStatus);
		MCommand::AddStringOption(s_szCode, StatusCode::R_NC_CONNECTCLOSED);
		MCommand::AddStringOption(s_szDescription, "Successfully Closed.");
		break;
	case CONNECT_FAILED:
		MCommand::SetCmdName(MCommand::_error);
		MCommand::AddStringOption(s_szLevel, s_szError);
		MCommand::AddStringOption(s_szCode, StatusCode::E_NC_CONNECTFAILED);
		MCommand::AddStringOption(s_szDescription, "Failed to connect application.");
		break;
	case CONNECT_REJECTED:
		MCommand::SetCmdName(MCommand::_error);
		MCommand::AddStringOption(s_szLevel, s_szError);
		MCommand::AddStringOption(s_szCode, StatusCode::E_NC_CONNECTREJECTED);
		MCommand::AddStringOption(s_szDescription, "You have no authority to access application.");
		break;
	case CONNECT_APPSHUTDOWN:
		MCommand::SetCmdName(MCommand::_error);
		MCommand::AddStringOption(s_szLevel, s_szError);
		MCommand::AddStringOption(s_szCode, StatusCode::E_NC_CONNECTAPPSHUTDOWN);
		MCommand::AddStringOption(s_szDescription, "Application is shutting down.");
		break;
	case CONNECT_INVALIDAPP:
		MCommand::SetCmdName(MCommand::_error);
		MCommand::AddStringOption(s_szLevel, s_szError);
		MCommand::AddStringOption(s_szCode, StatusCode::E_NC_CONNECTINVALIDAPP);
		MCommand::AddStringOption(s_szDescription, "Try to connect invalid application.");
		break;
	}
	MCommand::SetTransId(1);
	MCommand::AddStringProperty("fmsVer", strPruduct << "/" << strVersion);
	MCommand::AddDoubleProperty("capabilities", 31);
	MCommand::AddDoubleProperty("mode", 1);
}

MRConnect::~MRConnect()
{
	//
}

//////////////////////////////////////////////////////////////////////////
/// class MRStream
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(MRStream, 8);

MRStream::MRStream(uint32_t eStatus)
	: MCommand(0, 0, 0)
	, m_eStatusType(eStatus)
{
	switch(m_eStatusType)
	{
	case RTMP_S_NS_PLAYSTART:
		MCommand::SetCmdName(MCommand::onStatus);
		MCommand::AddStringOption(s_szLevel, s_szStatus);
		MCommand::AddStringOption(s_szCode, StatusCode::S_NS_PLAYSTART);
		break;
	case RTMP_S_NS_PLAYSTOP:
		MCommand::SetCmdName(MCommand::onStatus);
		MCommand::AddStringOption(s_szLevel, s_szStatus);
		MCommand::AddStringOption(s_szCode, StatusCode::S_NS_PLAYSTOP);
		break;
	case RTMP_S_NS_PLAYRESET:
		MCommand::SetCmdName(MCommand::onStatus);
		MCommand::AddStringOption(s_szLevel, s_szStatus);
		MCommand::AddStringOption(s_szCode, StatusCode::S_NS_PLAYRESET);
		break;
	case RTMP_S_NS_PLAYPUBLISHNOTIFY:
		MCommand::SetCmdName(MCommand::onStatus);
		MCommand::AddStringOption(s_szLevel, s_szStatus);
		MCommand::AddStringOption(s_szCode, StatusCode::S_NS_PLAYPUBLISHNOTIFY);
		break;
	case RTMP_S_NS_PLAYUNPUBLISHNOTIFY:
		MCommand::SetCmdName(MCommand::onStatus);
		MCommand::AddStringOption(s_szLevel, s_szStatus);
		MCommand::AddStringOption(s_szCode, StatusCode::S_NS_PLAYUNPUBLISHNOTIFY);
		break;
	case RTMP_S_NS_PLAYSWITCH:
		MCommand::SetCmdName(MCommand::onStatus);
		MCommand::AddStringOption(s_szLevel, s_szStatus);
		MCommand::AddStringOption(s_szCode, StatusCode::S_NS_PLAYSWITCH);
		break;
	case RTMP_S_NS_PLAYCOMPLETE:
		MCommand::SetCmdName(MCommand::onStatus);
		MCommand::AddStringOption(s_szLevel, s_szStatus);
		MCommand::AddStringOption(s_szCode, StatusCode::S_NS_PLAYCOMPLETE);
		break;
	case RTMP_S_NS_PLAYTRANSITIONCOMPLETE:
		MCommand::SetCmdName(MCommand::onStatus);
		MCommand::AddStringOption(s_szLevel, s_szStatus);
		MCommand::AddStringOption(s_szCode, StatusCode::S_NS_PLAYTRANSITIONCOMPLETE);
		break;
	case RTMP_S_NS_BUFFEREMPTY:
		MCommand::SetCmdName(MCommand::onStatus);
		MCommand::AddStringOption(s_szLevel, s_szStatus);
		MCommand::AddStringOption(s_szCode, StatusCode::S_NS_BUFFEREMPTY);
		break;
	case RTMP_S_NS_BUFFERFULL:
		MCommand::SetCmdName(MCommand::onStatus);
		MCommand::AddStringOption(s_szLevel, s_szStatus);
		MCommand::AddStringOption(s_szCode, StatusCode::S_NS_BUFFERFULL);
		break;
	case RTMP_S_NS_BUFFERFLUSH:
		MCommand::SetCmdName(MCommand::onStatus);
		MCommand::AddStringOption(s_szLevel, s_szStatus);
		MCommand::AddStringOption(s_szCode, StatusCode::S_NS_BUFFERFLUSH);
		break;
	case RTMP_E_NS_PLAYFAILED:
		MCommand::SetCmdName(MCommand::onStatus);
		MCommand::AddStringOption(s_szLevel, s_szError);
		MCommand::AddStringOption(s_szCode, StatusCode::E_NS_PLAYFAILED);
		break;
	case RTMP_E_NS_PLAYSTREAMNOTFOUND:
		MCommand::SetCmdName(MCommand::onStatus);
		MCommand::AddStringOption(s_szLevel, s_szError);
		MCommand::AddStringOption(s_szCode, StatusCode::E_NS_PLAYSTREAMNOTFOUND);
		break;
	case RTMP_E_NS_PLAYINSUFFICIENTBW:
		MCommand::SetCmdName(MCommand::onStatus);
		MCommand::AddStringOption(s_szLevel, s_szError);
		MCommand::AddStringOption(s_szCode, StatusCode::E_NS_PLAYINSUFFICIENTBW);
		break;
	case RTMP_E_NS_PLAYFILESTRUCTUREINVALID:
		MCommand::SetCmdName(MCommand::onStatus);
		MCommand::AddStringOption(s_szLevel, s_szError);
		MCommand::AddStringOption(s_szCode, StatusCode::E_NS_PLAYFILESTRUCTUREINVALID);
		break;
	case RTMP_E_NS_PLAYNOSUPPORTEDTRACKFOUND:
		MCommand::SetCmdName(MCommand::onStatus);
		MCommand::AddStringOption(s_szLevel, s_szError);
		MCommand::AddStringOption(s_szCode, StatusCode::E_NS_PLAYNOSUPPORTEDTRACKFOUND);
		break;
	case RTMP_S_NS_PUBLISHSTART:
		MCommand::SetCmdName(MCommand::onStatus);
		MCommand::AddStringOption(s_szLevel, s_szStatus);
		MCommand::AddStringOption(s_szCode, StatusCode::S_NS_PUBLISHSTART);
		break;
	case RTMP_E_NS_PUBLISHBADNAME:
		MCommand::SetCmdName(MCommand::onStatus);
		MCommand::AddStringOption(s_szLevel, s_szError);
		MCommand::AddStringOption(s_szCode, StatusCode::E_NS_PUBLISHBADNAME);
		break;
	case RTMP_S_NS_PUBLISHIDLE:
		MCommand::SetCmdName(MCommand::onStatus);
		MCommand::AddStringOption(s_szLevel, s_szStatus);
		MCommand::AddStringOption(s_szCode, StatusCode::S_NS_PUBLISHIDLE);
		break;
	case RTMP_S_NS_UNPUBLISHSUCCESS:
		MCommand::SetCmdName(MCommand::onStatus);
		MCommand::AddStringOption(s_szLevel, s_szStatus);
		MCommand::AddStringOption(s_szCode, StatusCode::S_NS_UNPUBLISHSUCCESS);
		break;
	case RTMP_E_NS_SEEKFAILED:
		MCommand::SetCmdName(MCommand::onStatus);
		MCommand::AddStringOption(s_szLevel, s_szError);
		MCommand::AddStringOption(s_szCode, StatusCode::E_NS_SEEKFAILED);
		break;
	case RTMP_E_NS_SEEKINVALIDTIME:
		MCommand::SetCmdName(MCommand::onStatus);
		MCommand::AddStringOption(s_szLevel, s_szError);
		MCommand::AddStringOption(s_szCode, StatusCode::E_NS_SEEKINVALIDTIME);
		break;
	case RTMP_S_NS_SEEKNOTIFY:
		MCommand::SetCmdName(MCommand::onStatus);
		MCommand::AddStringOption(s_szLevel, s_szStatus);
		MCommand::AddStringOption(s_szCode, StatusCode::S_NS_SEEKNOTIFY);
		break;
	case RTMP_S_NS_RECORDSTART:
		MCommand::SetCmdName(MCommand::onStatus);
		MCommand::AddStringOption(s_szLevel, s_szStatus);
		MCommand::AddStringOption(s_szCode, StatusCode::S_NS_RECORDSTART);
		break;
	case RTMP_E_NS_RECORDNOACCESS:
		MCommand::SetCmdName(MCommand::onStatus);
		MCommand::AddStringOption(s_szLevel, s_szError);
		MCommand::AddStringOption(s_szCode, StatusCode::E_NS_RECORDNOACCESS);
		break;
	case RTMP_S_NS_RECORDSTOP:
		MCommand::SetCmdName(MCommand::onStatus);
		MCommand::AddStringOption(s_szLevel, s_szStatus);
		MCommand::AddStringOption(s_szCode, StatusCode::S_NS_RECORDSTOP);
		break;
	case RTMP_E_NS_RECORDFAILED:
		MCommand::SetCmdName(MCommand::onStatus);
		MCommand::AddStringOption(s_szLevel, s_szError);
		MCommand::AddStringOption(s_szCode, StatusCode::E_NS_RECORDFAILED);
		break;
	case RTMP_S_NS_PAUSENOTIFY:
		MCommand::SetCmdName(MCommand::onStatus);
		MCommand::AddStringOption(s_szLevel, s_szStatus);
		MCommand::AddStringOption(s_szCode, StatusCode::S_NS_PAUSENOTIFY);
		break;
	case RTMP_E_NS_PAUSEFAILED:
		MCommand::SetCmdName(MCommand::onStatus);
		MCommand::AddStringOption(s_szLevel, s_szError);
		MCommand::AddStringOption(s_szCode, StatusCode::E_NS_PAUSEFAILED);
		break;
	case RTMP_S_NS_UNPAUSENOTIFY:
		MCommand::SetCmdName(MCommand::onStatus);
		MCommand::AddStringOption(s_szLevel, s_szStatus);
		MCommand::AddStringOption(s_szCode, StatusCode::S_NS_UNPAUSENOTIFY);
		break;
	}
}

MRStream::~MRStream()
{
	//
}

//////////////////////////////////////////////////////////////////////////
/// end of namespace RTMP
//////////////////////////////////////////////////////////////////////////

} // End of namespace RTMP

///////////////////////////////////////////////////////////////////////////////
// End of file : NetStatus.cpp
///////////////////////////////////////////////////////////////////////////////
