/*
	SuperCollider real time audio synthesis system
    Copyright (c) 2002 James McCartney. All rights reserved.
	http://www.audiosynth.com

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "OSCData.h"
#include "PyrPrimitive.h"
#include "PyrKernel.h"
#include "PyrInterpreter.h"
#include "PyrSched.h"
#include "GC.h"
//#include "PyrOMS.h"
//#include "MidiQ.h"
#include <string.h>
#include <math.h>
#include <stdexcept>
#include <new.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <pthread.h>
#include "scsynthsend.h"
#include "sc_msg_iter.h"
#include "SC_ComPort.h"
#include "SC_WorldOptions.h"

struct World *gLocalSynthServer = 0;

SC_UdpInPort* gUDPport = 0;

PyrString* newPyrString(VMGlobals *g, char *s, int flags, bool collect);

PyrSymbol *s_call, *s_write, *s_recvoscmsg, *s_recvoscbndl, *s_netaddr;
const char* gPassword;

#define USE_SCHEDULER 1


///////////

extern SC_UdpInPort* gUDPport;

scpacket gSynthPacket;

const int ivxNetAddr_Hostaddr = 0;
const int ivxNetAddr_PortID = 1;
const int ivxNetAddr_Hostname = 2;
const int ivxNetAddr_Socket = 3;

void makeSockAddr(struct sockaddr_in &toaddr, int32 addr, int32 port);
int sendallto(int socket, const void *msg, size_t len, struct sockaddr *toaddr, int addrlen);
int sendall(int socket, const void *msg, size_t len);

void addMsgSlot(scpacket *packet, PyrSlot *slot);
void addMsgSlot(scpacket *packet, PyrSlot *slot)
{
	switch (slot->utag) {
		case tagInt :
			packet->addi(slot->ui);
			break;
		case tagSym :
			packet->adds(slot->us->name);
			break;
		case tagObj :
			if (isKindOf(slot->uo, class_string)) {
				PyrString *stringObj = slot->uos;
				packet->adds(stringObj->s, stringObj->size);
			} else if (isKindOf(slot->uo, class_int8array)) {
				PyrInt8Array *arrayObj = slot->uob;
				packet->addb(arrayObj->b, arrayObj->size);
			}
			break;
		default :
			packet->addf(slot->uf);
			break;
	}
}

void addMsgSlotWithTags(scpacket *packet, PyrSlot *slot);
void addMsgSlotWithTags(scpacket *packet, PyrSlot *slot)
{
	switch (slot->utag) {
		case tagInt :
			packet->addtag('i');
			packet->addi(slot->ui);
			break;
		case tagSym :
			packet->addtag('s');
			packet->adds(slot->us->name);
			break;
		case tagObj :
			if (isKindOf(slot->uo, class_string)) {
				PyrString *stringObj = slot->uos;
				packet->addtag('s');
				packet->adds(stringObj->s, stringObj->size);
			} else if (isKindOf(slot->uo, class_int8array)) {
				PyrInt8Array *arrayObj = slot->uob;
				packet->addtag('b');
				printf("arrayObj %08X %d\n", arrayObj, arrayObj->size);
				packet->addb(arrayObj->b, arrayObj->size);
			}
			break;
		default :
			packet->addtag('f');
			packet->addf(slot->uf);
			break;
	}
}

int makeSynthMsg(scpacket *packet, PyrSlot *slots, int size);
int makeSynthMsg(scpacket *packet, PyrSlot *slots, int size)
{
	packet->BeginMsg();
	
	for (int i=0; i<size; ++i) {
		addMsgSlot(packet, slots+i);
	}
	
	packet->EndMsg();
	return errNone;
}

int makeSynthMsgWithTags(scpacket *packet, PyrSlot *slots, int size);
int makeSynthMsgWithTags(scpacket *packet, PyrSlot *slots, int size)
{
	packet->BeginMsg();
	
	//char *name = (char*)packet->wrpos;
	addMsgSlot(packet, slots);
	
	// skip space for tags
	//postfl("maketags %d %d\n", size, (size + 4) >> 2);
	packet->maketags(size);
	
        //char *tags = packet->tagwrpos;
	packet->addtag(',');

	for (int i=1; i<size; ++i) {
		addMsgSlotWithTags(packet, slots+i);
	}
	
	//postfl("pkt '%s' '%s'\n", name, tags);
	
	packet->EndMsg();

	return errNone;
}

void PerformOSCBundle(OSC_Packet* inPacket);
void PerformOSCMessage(int inSize, char *inData, ReplyAddress *inReply);

void localServerReplyFunc(struct ReplyAddress *inReplyAddr, char* inBuf, int inSize);
void localServerReplyFunc(struct ReplyAddress *inReplyAddr, char* inBuf, int inSize)
{
	post("localServerReplyFunc\n");
	OSC_Packet packet;
    packet.mIsBundle = strcmp(packet.mData, "#bundle") == 0;
    
    pthread_mutex_lock (&gLangMutex);
    if (packet.mIsBundle) {
		packet.mData = inBuf;
		packet.mSize = inSize;
		packet.mReplyAddr = *inReplyAddr;
        PerformOSCBundle(&packet);
    } else {
        PerformOSCMessage(inSize, inBuf, inReplyAddr);
    }
    pthread_mutex_unlock (&gLangMutex);
	
}

int makeSynthBundle(scpacket *packet, PyrSlot *slots, int size);
int makeSynthBundle(scpacket *packet, PyrSlot *slots, int size)
{
	double time;
	int err;
	int64 oscTime;
	
	err = slotDoubleVal(slots, &time);
	if (!err) {
		oscTime = ElapsedTimeToOSC(time);
	} else {
		oscTime = 1;	// immediate
	}
	packet->OpenBundle(oscTime);
	
	for (int i=1; i<size; ++i) {
		if (isKindOfSlot(slots+i, class_array)) {
			PyrObject *obj = slots[i].uo;
			makeSynthMsgWithTags(packet, obj->slots, obj->size);
		}
	}
	packet->CloseBundle();
	return errNone;
}

int netAddrSend(PyrObject *netAddrObj, int msglen, char *bufptr);
int netAddrSend(PyrObject *netAddrObj, int msglen, char *bufptr)
{
	if (gUDPport == 0) return errFailed;

	int err, port, addr, tcpSocket;
	
	err = slotIntVal(netAddrObj->slots + ivxNetAddr_Socket, &tcpSocket);
	if (err) return err;
	
	if (tcpSocket != -1) {
		// send TCP
		sendall(tcpSocket, bufptr, msglen);
		
	} else {
		// send UDP
		err = slotIntVal(netAddrObj->slots + ivxNetAddr_Hostaddr, &addr);
		if (err) return err;
		
		if (addr == 0) {
			if (gLocalSynthServer) {
				World_SendPacket(gLocalSynthServer, msglen, bufptr, &localServerReplyFunc);
			}
			return errNone;
		}

		err = slotIntVal(netAddrObj->slots + ivxNetAddr_PortID, &port);
		if (err) return err;		
		
		struct sockaddr_in toaddr;
		makeSockAddr(toaddr, addr, port);
	
		sendallto(gUDPport->Socket(), bufptr, msglen, (sockaddr*)&toaddr, sizeof(toaddr));
	}
	return errNone;
}


///////////
///////////

inline bool IsBundle(char* ptr) 
{ 
	return strcmp(ptr, "#bundle") == 0; 
}


inline int OSCStrLen(char *str) 
{
	return (strlen(str) + 4) & ~3;
}


int makeSynthMsgWithTags(scpacket *packet, PyrSlot *slots, int size);
int makeSynthBundle(scpacket *packet, PyrSlot *slots, int size);

int prNetAddr_Connect(VMGlobals *g, int numArgsPushed);
int prNetAddr_Connect(VMGlobals *g, int numArgsPushed)
{	
	PyrSlot* netAddrSlot = g->sp;
	PyrObject* netAddrObj = netAddrSlot->uo;
	
	int err, port, addr;
	
	err = slotIntVal(netAddrObj->slots + ivxNetAddr_PortID, &port);
	if (err) return err;
	
	err = slotIntVal(netAddrObj->slots + ivxNetAddr_Hostaddr, &addr);
	if (err) return err;
	
	struct sockaddr_in toaddr;
	makeSockAddr(toaddr, addr, port);
	
    int aSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (aSocket == -1) {
        post("\nCould not create socket\n");
		return errFailed;
	}
	
	const int on = 1;
	if (setsockopt( aSocket, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) != 0) {
        post("\nCould not setsockopt TCP_NODELAY\n");
		close(aSocket);
		return errFailed;
	};
	

    if(connect(aSocket,(struct sockaddr*)&toaddr,sizeof(toaddr)) != 0)
    {
        post("\nCould not connect socket\n");
		close(aSocket);
        return errFailed;
    }
	
	SetInt(netAddrObj->slots + ivxNetAddr_Socket, aSocket);

	return errNone;
}

int prNetAddr_Disconnect(VMGlobals *g, int numArgsPushed);
int prNetAddr_Disconnect(VMGlobals *g, int numArgsPushed)
{	
	PyrSlot* netAddrSlot = g->sp;
	PyrObject* netAddrObj = netAddrSlot->uo;
	
	int tcpSocket;
	int err = slotIntVal(netAddrObj->slots + ivxNetAddr_Socket, &tcpSocket);
	if (err) return err;
	
	if (tcpSocket != -1) close(tcpSocket);
	SetInt(netAddrObj->slots + ivxNetAddr_Socket, -1);
	
	return errNone;
}


int prNetAddr_SendMsg(VMGlobals *g, int numArgsPushed);
int prNetAddr_SendMsg(VMGlobals *g, int numArgsPushed)
{	
	PyrSlot* netAddrSlot = g->sp - numArgsPushed + 1;
	PyrSlot* args = netAddrSlot + 1;
	scpacket packet;
	
	int numargs = numArgsPushed - 1;
	makeSynthMsgWithTags(&packet, args, numargs);

	//for (int i=0; i<packet.size()/4; i++) post("%d %08X\n", i, packet.buf[i]);

	return netAddrSend(netAddrSlot->uo, packet.size(), (char*)packet.buf);
}

int prNetAddr_SendBundle(VMGlobals *g, int numArgsPushed);
int prNetAddr_SendBundle(VMGlobals *g, int numArgsPushed)
{	
	PyrSlot* netAddrSlot = g->sp - numArgsPushed + 1;
	PyrSlot* args = netAddrSlot + 1;
	scpacket packet;
	
	double time;
	int err = slotDoubleVal(args, &time);
	if (!err) {
		time += g->thread->time.uf;
		SetFloat(args, time);
	}
	int numargs = numArgsPushed - 1;
	makeSynthBundle(&packet, args, numargs);
	
	//for (int i=0; i<packet.size()/4; i++) post("%d %08X\n", i, packet.buf[i]);
	
	return netAddrSend(netAddrSlot->uo, packet.size(), (char*)packet.buf);
}

int prNetAddr_SendRaw(VMGlobals *g, int numArgsPushed);
int prNetAddr_SendRaw(VMGlobals *g, int numArgsPushed)
{	
	PyrSlot* netAddrSlot = g->sp - 1;
	PyrSlot* arraySlot = g->sp;
	PyrObject* netAddrObj = netAddrSlot->uo;
	
	if (!IsObj(arraySlot) || !isKindOf(arraySlot->uo, class_rawarray)) {
		error("sendRaw arg must be a kind of RawArray.\n");
		return errWrongType;
	}
	PyrObject *array = arraySlot->uo;
	
	char *bufptr = (char*)array->slots;
	int32 msglen = array->size * gFormatElemSize[array->obj_format];
	
	return netAddrSend(netAddrObj, msglen, bufptr);
}

PyrObject* ConvertOSCMessage(int inSize, char *inData)
{
        char *cmdName = inData;
	int cmdNameLen = OSCstrlen(cmdName);
	sc_msg_iter msg(inSize - cmdNameLen, inData + cmdNameLen);
        
	int numElems;
        if (inSize == cmdNameLen) {
            numElems = 0;
        } else {
            numElems = strlen(msg.tags);
        }
        //post("tags %s %d\n", msg.tags, numElems);
        
        VMGlobals *g = gMainVMGlobals;
        PyrObject *obj = newPyrArray(g->gc, numElems + 1, 0, false);
        PyrSlot *slots = obj->slots;

        SetSymbol(slots+0, getsym(cmdName));
        
        for (int i=0; i<numElems; ++i) {
            char tag = msg.nextTag();
            //post("%d %c\n", i, tag);
            switch (tag) {
                case 'i' :
                    SetInt(slots+i+1, msg.geti());
                    break;
                case 'f' :
                    SetFloat(slots+i+1, msg.getf());
                    break;
                case 's' :
                    SetSymbol(slots+i+1, getsym(msg.gets()));
                    //post("sym '%s'\n", slots[i+1].us->name);
                    break;
                case 'b' :
                    SetObject(slots+i+1, getsym(msg.gets()));
                    //post("sym '%s'\n", slots[i+1].us->name);
                    break;
            }
        }
        obj->size = numElems + 1;
        return obj;
}

PyrObject* ConvertReplyAddress(ReplyAddress *inReply)
{
    VMGlobals *g = gMainVMGlobals;
    PyrObject *obj = instantiateObject(g->gc, s_netaddr->u.classobj, 2, true, false);
    PyrSlot *slots = obj->slots;
    SetInt(slots+0, inReply->mSockAddr.sin_addr.s_addr);
    SetInt(slots+1, inReply->mSockAddr.sin_port);
    return obj;
}

void PerformOSCBundle(OSC_Packet* inPacket)
{
    PyrObject *replyObj = ConvertReplyAddress(&inPacket->mReplyAddr);
    // convert all data to arrays
    
    int64 oscTime;
    memcpy(&oscTime, inPacket->mData + 8, sizeof(int64)); 
    // need to byte swap oscTime if host is little endian..
    
    double seconds = OSCToElapsedTime(oscTime);

    VMGlobals *g = gMainVMGlobals;
    ++g->sp; SetObject(g->sp, g->process);
    ++g->sp; SetObject(g->sp, replyObj);
    ++g->sp; SetFloat(g->sp, seconds);
    
    int numMsgs = 0;
    char *data = inPacket->mData + 16;
    char* dataEnd = inPacket->mData + inPacket->mSize;
    while (data < dataEnd) {
        int32 msgSize = *(int32*)data;
        data += sizeof(int32);
        PyrObject *arrayObj = ConvertOSCMessage(msgSize, data);
        ++g->sp; SetObject(g->sp, arrayObj);
        numMsgs++;
        data += msgSize;
    }

    runInterpreter(g, s_recvoscbndl, 3+numMsgs);
}

void PerformOSCMessage(int inSize, char *inData, ReplyAddress *inReply)
{
    
    PyrObject *replyObj = ConvertReplyAddress(inReply);
    PyrObject *arrayObj = ConvertOSCMessage(inSize, inData);
    
    // call virtual machine to handle message
    VMGlobals *g = gMainVMGlobals;
    ++g->sp; SetObject(g->sp, g->process);
    ++g->sp; SetNil(g->sp);	// time
    ++g->sp; SetObject(g->sp, replyObj);
    ++g->sp; SetObject(g->sp, arrayObj);
    runInterpreter(g, s_recvoscmsg, 4);

}

void FreeOSCPacket(OSC_Packet *inPacket)
{
    //post("->FreeOSCPacket %08X\n", inPacket);
    if (inPacket) {
            free(inPacket->mData);
            free(inPacket);
    }
}

void ProcessOSCPacket(OSC_Packet* inPacket)
{
    //post("recv '%s' %d\n", inPacket->mData, inPacket->mSize);
    inPacket->mIsBundle = strcmp(inPacket->mData, "#bundle") == 0;
    
    pthread_mutex_lock (&gLangMutex);
    if (inPacket->mIsBundle) {
        PerformOSCBundle(inPacket);
    } else {
        PerformOSCMessage(inPacket->mSize, inPacket->mData, &inPacket->mReplyAddr);
    }
    pthread_mutex_unlock (&gLangMutex);

    FreeOSCPacket(inPacket);
}

void init_OSC(int port);
void init_OSC(int port)
{	
    postfl("init_OSC\n");
    try {
        gUDPport = new SC_UdpInPort(port);
    } catch (...) {
        postfl("No networking.");
    }
}

int prGetHostByName(VMGlobals *g, int numArgsPushed);
int prGetHostByName(VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a = g->sp;
	char hostname[256];
	
	int err = slotStrVal(a, hostname, 255);
	if (err) return err;
		
	struct hostent *he = gethostbyname(hostname);
	if (!he) return errFailed;
	
	SetInt(a, *(int*)he->h_addr);
	postfl("prGetHostByName hostname %s addr %d\n", hostname, a->ui);
	
	return errNone;
}

int prExit(VMGlobals *g, int numArgsPushed);
int prExit(VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a = g->sp;
	
        //exit(a->ui);
        
        post("exit %d\n", a->ui);
        DumpBackTrace(g);
	return errNone;
}


int prBootInProcessServer(VMGlobals *g, int numArgsPushed);
int prBootInProcessServer(VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a = g->sp;
	
	if (!gLocalSynthServer) {
		WorldOptions options = kDefaultWorldOptions;
		gLocalSynthServer = World_New(&options);
	}
	return errNone;
}

void* wait_for_quit(void* thing);
void* wait_for_quit(void* thing)
{
	World *world = (World*)thing;
	World_WaitForQuit(world);
	return 0;
}

int prQuitInProcessServer(VMGlobals *g, int numArgsPushed);
int prQuitInProcessServer(VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a = g->sp;
	
	if (gLocalSynthServer) {
		World *world = gLocalSynthServer;
		gLocalSynthServer = 0;
		
        pthread_t thread;
        pthread_create(&thread, NULL, wait_for_quit, (void*)world);
		pthread_detach(thread);
	}
	
	return errNone;
}



void init_OSC_primitives();
void init_OSC_primitives()
{
	int base, index;
	
	base = nextPrimitiveIndex();
	index = 0;

	definePrimitive(base, index++, "_NetAddr_Connect", prNetAddr_Connect, 1, 0);	
	definePrimitive(base, index++, "_NetAddr_Disconnect", prNetAddr_Disconnect, 1, 0);	
	definePrimitive(base, index++, "_NetAddr_SendMsg", prNetAddr_SendMsg, 1, 1);	
	definePrimitive(base, index++, "_NetAddr_SendBundle", prNetAddr_SendBundle, 2, 1);	
	definePrimitive(base, index++, "_NetAddr_SendRaw", prNetAddr_SendRaw, 2, 0);	
	definePrimitive(base, index++, "_GetHostByName", prGetHostByName, 1, 0);	
	definePrimitive(base, index++, "_Exit", prExit, 1, 0);	
	definePrimitive(base, index++, "_BootInProcessServer", prBootInProcessServer, 1, 0);	
	definePrimitive(base, index++, "_QuitInProcessServer", prQuitInProcessServer, 1, 0);	

	//post("initOSCRecs###############\n");
	s_call = getsym("call");
	s_write = getsym("write");
	s_recvoscmsg = getsym("recvOSCmessage");
	s_recvoscbndl = getsym("recvOSCbundle");
	s_netaddr = getsym("NetAddr");
}

