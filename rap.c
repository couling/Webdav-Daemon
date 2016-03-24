#include "shared.h"
#include "xml.h"

//#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <locale.h>
#include <security/pam_appl.h>

#define WEBDAV_NAMESPACE "DAV:"
#define EXTENSIONS_NAMESPACE "urn:couling-webdav:"
#define MICROSOFT_NAMESPACE "urn:schemas-microsoft-com:"

#define NEW_FILE_PERMISSIONS 0666
#define NEW_DIR_PREMISSIONS  0777

typedef struct MimeType {
	const char * fileExtension;
	const char * type;
	size_t typeStringSize;
} MimeType;

// Authentication
static int authenticated = 0;
static const char * authenticatedUser;
static const char * pamService;
static pam_handle_t *pamh;

// Mime Database.
static size_t mimeFileBufferSize;
static char * mimeFileBuffer;
static MimeType * mimeTypes = NULL;
static int mimeTypeCount = 0;

static MimeType UNKNOWN_MIME_TYPE = {
		.fileExtension = "",
		.type = "application/octet-stream",
		.typeStringSize = sizeof("application/octet-stream") };

static MimeType XML_MIME_TYPE = {
		.fileExtension = "",
		.type = "application/xml; charset=utf-8",
		.typeStringSize = sizeof("application/xml; charset=utf-8") };

static ssize_t respond(RapConstant result) {
	Message message = { .mID = result, .fd = -1, .paramCount = 0 };
	return sendMessage(RAP_CONTROL_SOCKET, &message);
}

static char * normalizeDirName(const char * file, size_t * filePathSize, int isDir) {
	char * filePath = mallocSafe(*filePathSize + 2);
	memcpy(filePath, file, *filePathSize + 1);
	if (isDir && file[*filePathSize - 1] != '/') {
		filePath[*filePathSize] = '/';
		filePath[*filePathSize + 1] = '\0';
		(*filePathSize)++;
	}
	return filePath;
}

static size_t formatFileSize(char * buffer, size_t bufferSize, off_t size) {
	static char * suffix[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" "EiB", "ZiB", "YiB" };
	int magnitude = 0;
	off_t tmpSize = size;
	while (magnitude < 8 && (tmpSize & 1023) != tmpSize) {
		magnitude++;
		tmpSize >>= 10;
	}
	double divisor;
	char * format;
	if (magnitude > 0) {
		divisor = ((off_t) 1) << (magnitude * 10);
		if (tmpSize >= 100) {
			format = "%.0f %s";
		} else if (tmpSize >= 10) {
			format = "%.1f %s";
		} else {
			format = "%.2f %s";
		}
	} else {
		divisor = 1;
		format = "%.0f %s";
	}
	double dsize = size;
	dsize /= divisor;
	return snprintf(buffer, bufferSize, format, dsize, suffix[magnitude]);
}

//////////
// Mime //
//////////

static int compareExt(const void * a, const void * b) {
	return strcmp(((MimeType *) a)->fileExtension, ((MimeType *) b)->fileExtension);
}

static MimeType * findMimeType(const char * file) {

	if (!file) {
		return &UNKNOWN_MIME_TYPE;
	}
	MimeType type;
	type.fileExtension = file + strlen(file) - 1;
	while (1) {
		if (*type.fileExtension == '/') {
			return &UNKNOWN_MIME_TYPE;
		} else if (*type.fileExtension == '.') {
			type.fileExtension++;
			break;
		} else {
			type.fileExtension--;
			if (type.fileExtension < file) {
				return &UNKNOWN_MIME_TYPE;
			}
		}
	}

	MimeType * result = bsearch(&type, mimeTypes, mimeTypeCount, sizeof(*mimeTypes), &compareExt);
	return result ? result : &UNKNOWN_MIME_TYPE;
}

static void initializeMimeTypes(const char * mimeTypesFile) {
	// Load Mime file into memory
	mimeFileBuffer = loadFileToBuffer(mimeTypesFile, &mimeFileBufferSize);
	if (!mimeFileBuffer) {
		exit(1);
	}

	// Parse mimeFile;
	char * partStartPtr = mimeFileBuffer;
	int found;
	char * type = NULL;
	do {
		found = 0;
		// find the start of the part
		while (partStartPtr < mimeFileBuffer + mimeFileBufferSize && !found) {
			switch (*partStartPtr) {
			case '#':
				// skip to the end of the line
				while (partStartPtr < mimeFileBuffer + mimeFileBufferSize && *partStartPtr != '\n') {
					partStartPtr++;
				}
				// Fall through to incrementing partStartPtr
				partStartPtr++;
				break;
			case ' ':
			case '\t':
			case '\r':
			case '\n':
				if (*partStartPtr == '\n') {
					type = NULL;
				}
				partStartPtr++;
				break;
			default:
				found = 1;
				break;
			}
		}

		// Find the end of the part
		char * partEndPtr = partStartPtr + 1;
		found = 0;
		while (partEndPtr < mimeFileBuffer + mimeFileBufferSize && !found) {
			switch (*partEndPtr) {
			case ' ':
			case '\t':
			case '\r':
			case '\n':
				if (type == NULL) {
					type = partStartPtr;
				} else {
					mimeTypes = reallocSafe(mimeTypes, sizeof(*mimeTypes) * (mimeTypeCount + 1));
					mimeTypes[mimeTypeCount].type = type;
					mimeTypes[mimeTypeCount].fileExtension = partStartPtr;
					mimeTypes[mimeTypeCount].typeStringSize = partEndPtr - partStartPtr + 1;
					mimeTypeCount++;
				}
				if (*partEndPtr == '\n') {
					type = NULL;
				}
				*partEndPtr = '\0';
				found = 1;
				break;
			default:
				partEndPtr++;
				break;
			}
		}
		partStartPtr = partEndPtr + 1;
	} while (partStartPtr < mimeFileBuffer + mimeFileBufferSize);

	qsort(mimeTypes, mimeTypeCount, sizeof(*mimeTypes), &compareExt);
}

//////////////
// End Mime //
//////////////

////////////////////
// Error Response //
////////////////////

static ssize_t writeErrorResponse(RapConstant responseCode, const char * textError, const char * error,
		const char * file) {
	int pipeEnds[2];
	if (pipe(pipeEnds)) {
		stdLogError(errno, "Could not create pipe to write content");
		return respond(RAP_RESPOND_INTERNAL_ERROR);
	}

	time_t fileTime;
	time(&fileTime);
	Message message = { .mID = responseCode, .fd = pipeEnds[PIPE_READ], .paramCount = 2 };
	message.params[RAP_PARAM_RESPONSE_DATE].iov_base = &fileTime;
	message.params[RAP_PARAM_RESPONSE_DATE].iov_len = sizeof(fileTime);
	message.params[RAP_PARAM_RESPONSE_MIME].iov_base = (void *) XML_MIME_TYPE.type;
	message.params[RAP_PARAM_RESPONSE_MIME].iov_len = XML_MIME_TYPE.typeStringSize;
	message.params[RAP_PARAM_RESPONSE_LOCATION] = stringToMessageParam(file);

	ssize_t messageResult = sendMessage(RAP_CONTROL_SOCKET, &message);
	if (messageResult <= 0) {
		close(pipeEnds[PIPE_WRITE]);
		return messageResult;
	}

	// We've set up the pipe and sent read end across so now write the result
	xmlTextWriterPtr writer = xmlNewFdTextWriter(pipeEnds[PIPE_WRITE]);
	xmlTextWriterStartDocument(writer, "1.0", "utf-8", NULL);
	xmlTextWriterStartElementNS(writer, "d", "error", WEBDAV_NAMESPACE);
	xmlTextWriterWriteAttributeNS(writer, "xmlns", "x", NULL, EXTENSIONS_NAMESPACE);
	if (error) {
		xmlTextWriterStartElementNS(writer, "d", error, NULL);
		xmlTextWriterStartElementNS(writer, "d", "href", NULL);
		xmlTextWriterWriteURL(writer, file);
		xmlTextWriterEndElement(writer);
		xmlTextWriterEndElement(writer);
	}
	if (textError) {
		xmlTextWriterStartElementNS(writer, "x", "text-error", NULL);
		xmlTextWriterStartElementNS(writer, "x", "href", NULL);
		xmlTextWriterWriteURL(writer, file);
		xmlTextWriterWriteElementString(writer, "x", "text", textError);
		xmlTextWriterEndElement(writer);
		xmlTextWriterEndElement(writer);
	}

	xmlTextWriterEndElement(writer);
	xmlFreeTextWriter(writer);
	return messageResult;
}

////////////////////////
// End Error Response //
////////////////////////

//////////
// LOCK //
//////////

typedef struct LockRequest {
	int isNewLock;
	LockType type;
} LockRequest;

static void parseLockRequest(int fd, LockRequest * lockRequest) {
	memset(lockRequest, 0, sizeof(LockRequest));
	if (fd == -1) return;
	xmlTextReaderPtr reader = xmlReaderForFd(fd, NULL, NULL, XML_PARSE_NOENT);
	xmlReaderSuppressErrors(reader);

	if (!reader || !stepInto(reader) || !elementMatches(reader, WEBDAV_NAMESPACE, "lockinfo")) {
		if (reader) xmlFreeTextReader(reader);
		close(fd);
		return;
	}

	lockRequest->isNewLock = 1;
	int readResult = stepInto(reader);
	while (readResult && xmlTextReaderDepth(reader) == 1) {
		if (isNamespaceElement(reader, WEBDAV_NAMESPACE)) {
			const char * nodeName = xmlTextReaderConstLocalName(reader);
			if (!strcmp(nodeName, "lockscope")) {
				readResult = stepInto(reader);
				while (readResult && xmlTextReaderDepth(reader) == 2) {
					if (isNamespaceElement(reader, WEBDAV_NAMESPACE)) {
						nodeName = xmlTextReaderConstLocalName(reader);
						if (!strcmp(nodeName, "shared")) {
							if (lockRequest->type != LOCK_TYPE_EXCLUSIVE) {
								lockRequest->type = LOCK_TYPE_SHARED;
							}
						} else if (!strcmp(nodeName, "exclusive")) {
							lockRequest->type = LOCK_TYPE_EXCLUSIVE;
						}
					}
					readResult = stepOver(reader);
				}
			} else if (!strcmp(nodeName, "locktype")) {
				readResult = stepInto(reader);
				while (readResult && xmlTextReaderDepth(reader) == 2) {
					if (isNamespaceElement(reader, WEBDAV_NAMESPACE)) {
						nodeName = xmlTextReaderConstLocalName(reader);
						if (!strcmp(nodeName, "read") && lockRequest->type != LOCK_TYPE_EXCLUSIVE) {
							lockRequest->type = LOCK_TYPE_SHARED;
						} else if (!strcmp(nodeName, "write")) {
							lockRequest->type = LOCK_TYPE_EXCLUSIVE;
						}
					}
					readResult = stepOver(reader);
				}
			} else {
				readResult = stepOver(reader);
			}
		}
	}

	// finish up
	while (readResult) {
		readResult = stepOver(reader);
	}

	xmlFreeTextReader(reader);
	close(fd);
}

static ssize_t writeLockResponse(const char * fileName, LockRequest * request, const char * lockToken,
		time_t timeout) {
	int pipeEnds[2];
	if (pipe(pipeEnds)) {
		stdLogError(errno, "Could not create pipe to write content");
		return respond(RAP_RESPOND_INTERNAL_ERROR);
	}

	time_t fileTime;
	time(&fileTime);
	Message message = { .mID = RAP_RESPOND_OK, .fd = pipeEnds[PIPE_READ], .paramCount = 2 };
	message.params[RAP_PARAM_RESPONSE_DATE].iov_base = &fileTime;
	message.params[RAP_PARAM_RESPONSE_DATE].iov_len = sizeof(fileTime);
	message.params[RAP_PARAM_RESPONSE_MIME].iov_base = (void *) XML_MIME_TYPE.type;
	message.params[RAP_PARAM_RESPONSE_MIME].iov_len = XML_MIME_TYPE.typeStringSize;
	message.params[RAP_PARAM_RESPONSE_LOCATION] = stringToMessageParam(fileName);

	ssize_t messageResult = sendMessage(RAP_CONTROL_SOCKET, &message);
	if (messageResult <= 0) {
		close(pipeEnds[PIPE_WRITE]);
		return messageResult;
	}

	// We've set up the pipe and sent read end across so now write the result
	xmlTextWriterPtr writer = xmlNewFdTextWriter(pipeEnds[PIPE_WRITE]);
	xmlTextWriterStartDocument(writer, "1.0", "utf-8", NULL);
	xmlTextWriterStartElementNS(writer, "d", "prop", WEBDAV_NAMESPACE);
	xmlTextWriterStartElementNS(writer, "d", "lockdiscovery", NULL);
	xmlTextWriterStartElementNS(writer, "d", "activelock", NULL);
	// <d:locktype><d:write></d:locktype>
	xmlTextWriterStartElementNS(writer, "d", "locktype", NULL);
	xmlTextWriterWriteElementString(writer, "d", (request->type == LOCK_TYPE_EXCLUSIVE ? "write" : "read"),
	NULL);
	xmlTextWriterEndElement(writer);
	// <d:lockscope><d:exclusive></d:lockscope>
	xmlTextWriterStartElementNS(writer, "d", "lockscope", NULL);
	xmlTextWriterWriteElementString(writer, "d",
			(request->type == LOCK_TYPE_EXCLUSIVE ? "exclusive" : "shared"), NULL);
	xmlTextWriterEndElement(writer);
	// <d:depth>Infinity</d:depth>
	xmlTextWriterWriteElementString(writer, "d", "depth", "infinity");
	// <d:owner>Bob</d:owner>
	// TODO check that this owner format is valid it may not be
	xmlTextWriterWriteElementString(writer, "d", "owner", authenticatedUser);
	// <d:lockroot><d:href>/foo/bar</d:lockroot></d:href>
	xmlTextWriterStartElementNS(writer, "d", "lockroot", NULL);
	xmlTextWriterStartElementNS(writer, "d", "href", NULL);
	xmlTextWriterWriteURL(writer, fileName);
	xmlTextWriterEndElement(writer);
	xmlTextWriterEndElement(writer);
	// <d:locktoken><d:href>urn:uuid:e71d4fae-5dec-22d6-fea5-00a0c91e6be4</d:href></d:locktoken>
	xmlTextWriterStartElementNS(writer, "d", "locktoken", NULL);
	xmlTextWriterStartElementNS(writer, "d", "href", NULL);
	xmlTextWriterWriteFormatString(writer, LOCK_TOKEN_URN_PREFIX "%s", lockToken);
	xmlTextWriterEndElement(writer);
	xmlTextWriterEndElement(writer);

	xmlTextWriterStartElement(writer, "d:timeout");
	xmlTextWriterWriteFormatString(writer, "Second-%d", (int) timeout);
	xmlTextWriterEndElement(writer);

	xmlTextWriterEndElement(writer);
	xmlTextWriterEndElement(writer);
	xmlTextWriterEndElement(writer);

	xmlFreeTextWriter(writer);
	return messageResult;
}

static ssize_t lockFile(Message * message) {
	const char * file = messageParamToString(&message->params[RAP_PARAM_REQUEST_FILE]);
	const char * lockToken = messageParamToString(&message->params[RAP_PARAM_REQUEST_LOCK]);
	//const char * depth = messageParamToString(&message->params[RAP_PARAM_REQUEST_DEPTH]);
	//if (depth == NULL) depth = "infinity";
	respond(RAP_RESPOND_CONTINUE);

	LockRequest lockRequest;
	parseLockRequest(message->fd, &lockRequest);

	Message interimMessage;
	char incomingBuffer[INCOMING_BUFFER_SIZE];
	ssize_t ioResponse;

	if (lockRequest.isNewLock) {
		if (lockToken) {
			// Lock token must be empty but isn't
			stdLogError(0, "lock-token header provided for new lock");
			return writeErrorResponse(RAP_RESPOND_BAD_CLIENT_REQUEST,
					"lock-token header provided for new lock", "lock-token-submitted", file);
		}

		int openFlags = (lockRequest.type == LOCK_TYPE_EXCLUSIVE ? O_WRONLY | O_CREAT : O_RDONLY);
		interimMessage.fd = open(file, openFlags, NEW_FILE_PERMISSIONS);
		if (interimMessage.fd == -1) {
			int e = errno;
			stdLogError(e, "Could not open file for lock %s", file);
			switch (e) {
			case EACCES:
				return writeErrorResponse(RAP_RESPOND_ACCESS_DENIED, strerror(e), NULL, file);
			case ENOENT:
				return writeErrorResponse(RAP_RESPOND_NOT_FOUND, strerror(e), NULL, file);
			default:
				return writeErrorResponse(RAP_RESPOND_NOT_FOUND, strerror(e), NULL, file);
			}
		}

		struct stat s;
		fstat(interimMessage.fd, &s);
		if ((s.st_mode & S_IFMT) != S_IFREG) {
			stdLogError(0, "Refusing to lock non-regular file %s", file);
			close(interimMessage.fd);
			return writeErrorResponse(RAP_RESPOND_CONFLICT, "Refusing to non-regular file", NULL, file);
		}

		if (flock(interimMessage.fd, lockRequest.type) == -1) {
			int e = errno;
			stdLogError(e, "Could not lock file %s", file);
			close(interimMessage.fd);
			return writeErrorResponse(RAP_RESPOND_LOCKED, strerror(e), "no-conflicting-lock", file);
		}

		interimMessage.mID = RAP_INTERIM_RESPOND_LOCK;
		interimMessage.paramCount = 2;
		interimMessage.params[RAP_PARAM_LOCK_LOCATION] = message->params[RAP_PARAM_REQUEST_FILE];
		interimMessage.params[RAP_PARAM_LOCK_TYPE].iov_base = &lockRequest.type;
		interimMessage.params[RAP_PARAM_LOCK_TYPE].iov_len = sizeof(lockRequest.type);
	} else {
		if (!lockToken) {
			// Lock token must not be empty
			return writeErrorResponse(RAP_RESPOND_BAD_CLIENT_REQUEST,
					"No lock tokent submitted for refresh request", "lock-token-submitted", file);
		}
		interimMessage.mID = RAP_INTERIM_RESPOND_RELOCK;
		interimMessage.fd = -1;
		interimMessage.paramCount = 2;
		interimMessage.params[RAP_PARAM_LOCK_LOCATION] = message->params[RAP_PARAM_REQUEST_FILE];
		interimMessage.params[RAP_PARAM_LOCK_TOKEN] = stringToMessageParam(lockToken);
	}

	ioResponse = sendRecvMessage(RAP_CONTROL_SOCKET, &interimMessage, incomingBuffer,
	INCOMING_BUFFER_SIZE);
	if (ioResponse <= 0) return ioResponse;

	if (interimMessage.mID == RAP_COMPLETE_REQUEST_LOCK) {
		lockToken = messageParamToString(&interimMessage.params[RAP_PARAM_LOCK_TOKEN]);
		time_t timeout = *((time_t *) interimMessage.params[RAP_PARAM_LOCK_TIMEOUT].iov_base);
		return writeLockResponse(file, &lockRequest, lockToken, timeout);
	} else {
		const char * reason = messageParamToString(&interimMessage.params[RAP_PARAM_ERROR_REASON]);
		//const char * errorNamespace = messageParamToString(&interimMessage.params[RAP_PARAM_ERROR_NAMESPACE]);

		return writeErrorResponse(interimMessage.mID, reason, NULL, file);
	}

}

//////////////
// End Lock //
//////////////

//////////////
// PROPFIND //
//////////////

#define PROPFIND_RESOURCE_TYPE "resourcetype"
#define PROPFIND_CREATION_DATE "creationdate"
#define PROPFIND_CONTENT_LENGTH "getcontentlength"
#define PROPFIND_LAST_MODIFIED "getlastmodified"
#define PROPFIND_DISPLAY_NAME "displayname"
#define PROPFIND_CONTENT_TYPE "getcontenttype"
#define PROPFIND_USED_BYTES "quota-used-bytes"
#define PROPFIND_AVAILABLE_BYTES "quota-available-bytes"
#define PROPFIND_ETAG "getetag"
#define PROPFIND_WINDOWS_ATTRIBUTES "Win32FileAttributes"

typedef struct PropertySet {
	char creationDate;
	char displayName;
	char contentLength;
	char contentType;
	char etag;
	char lastModified;
	char resourceType;
	char usedBytes;
	char availableBytes;
	char windowsHidden;
} PropertySet;

static int parsePropFind(int fd, PropertySet * properties) {
	xmlTextReaderPtr reader = xmlReaderForFd(fd, NULL, NULL, XML_PARSE_NOENT);
	xmlReaderSuppressErrors(reader);

	int readResult;
	if (!reader || !stepInto(reader)) {
		stdLogError(0, "could not create xml reader");
		if (reader) xmlFreeTextReader(reader);
		close(fd);
		return 0;
	}

	if (xmlTextReaderNodeType(reader) == XML_READER_TYPE_NONE) {
		// No body has been sent
		// so assume the client is asking for everything.
		memset(properties, 1, sizeof(*properties));
		xmlFreeTextReader(reader);
		close(fd);
		return 1;
	} else {
		memset(properties, 0, sizeof(PropertySet));
	}

	if (!elementMatches(reader, WEBDAV_NAMESPACE, "propfind")) {
		stdLogError(0, "Request body was not a propfind document");
		xmlFreeTextReader(reader);
		close(fd);
		return 0;
	}

	readResult = stepInto(reader);
	while (readResult && xmlTextReaderDepth(reader) > 0 && !elementMatches(reader, WEBDAV_NAMESPACE, "prop")) {
		stepOver(reader);
	}

	if (!readResult) {
		xmlFreeTextReader(reader);
		close(fd);
		return 0;
	}

	readResult = stepInto(reader);
	while (readResult && xmlTextReaderDepth(reader) > 1) {
		if (!strcmp(xmlTextReaderConstNamespaceUri(reader), WEBDAV_NAMESPACE)) {
			const char * nodeName = xmlTextReaderConstLocalName(reader);
			if (!strcmp(nodeName, PROPFIND_RESOURCE_TYPE)) {
				properties->resourceType = 1;
			} else if (!strcmp(nodeName, PROPFIND_CREATION_DATE)) {
				properties->creationDate = 1;
			} else if (!strcmp(nodeName, PROPFIND_CONTENT_LENGTH)) {
				properties->contentLength = 1;
			} else if (!strcmp(nodeName, PROPFIND_LAST_MODIFIED)) {
				properties->lastModified = 1;
			} else if (!strcmp(nodeName, PROPFIND_DISPLAY_NAME)) {
				properties->displayName = 1;
			} else if (!strcmp(nodeName, PROPFIND_CONTENT_TYPE)) {
				properties->contentType = 1;
			} else if (!strcmp(nodeName, PROPFIND_AVAILABLE_BYTES)) {
				properties->availableBytes = 1;
			} else if (!strcmp(nodeName, PROPFIND_USED_BYTES)) {
				properties->usedBytes = 1;
			} else if (!strcmp(nodeName, PROPFIND_ETAG)) {
				properties->etag = 1;
			}
		} else if (!strcmp(xmlTextReaderConstNamespaceUri(reader), MICROSOFT_NAMESPACE)) {
			const char * nodeName = xmlTextReaderConstLocalName(reader);
			if (!strcmp(nodeName, PROPFIND_WINDOWS_ATTRIBUTES)) {
				properties->windowsHidden = 1;
			}
		}
		readResult = stepOver(reader);
	}

	readResult = 1;

	// finish up
	while (stepOver(reader))
		// consume the rest of the input
		;

	xmlFreeTextReader(reader);
	close(fd);
	return readResult;
}

static void writePropFindResponsePart(const char * fileName, const char * displayName,
		PropertySet * properties, struct stat * fileStat, xmlTextWriterPtr writer) {

	xmlTextWriterStartElementNS(writer, "d", "response", NULL);
	xmlTextWriterStartElementNS(writer, "d", "href", NULL);
	xmlTextWriterWriteURL(writer, fileName);
	xmlTextWriterEndElement(writer);
	xmlTextWriterStartElementNS(writer, "d", "propstat", NULL);
	xmlTextWriterStartElementNS(writer, "d", "prop", NULL);

	if (properties->etag) {
		char buffer[200];
		snprintf(buffer, sizeof(buffer), "%lld-%lld", (long long) fileStat->st_size,
				(long long) fileStat->st_mtime);
		xmlTextWriterWriteElementString(writer, "d", PROPFIND_ETAG, buffer);
	}
	if (properties->creationDate) {
		char buffer[100];
		getWebDate(fileStat->st_ctime, buffer, 100);
		xmlTextWriterWriteElementString(writer, "d", PROPFIND_CREATION_DATE, buffer);
	}
	if (properties->lastModified) {
		char buffer[100];
		getWebDate(fileStat->st_ctime, buffer, 100);
		xmlTextWriterWriteElementString(writer, "d", PROPFIND_LAST_MODIFIED, buffer);
	}
	if (properties->resourceType) {
		xmlTextWriterStartElementNS(writer, "d", PROPFIND_RESOURCE_TYPE, NULL);
		if ((fileStat->st_mode & S_IFMT) == S_IFDIR) {
			xmlTextWriterStartElementNS(writer, "d", "collection", NULL);
			xmlTextWriterEndElement(writer);
		}
		xmlTextWriterEndElement(writer);
	}
	//if (properties->displayName) {
	//	xmlTextWriterWriteElementString(writer, PROPFIND_DISPLAY_NAME, displayName);
	//}
	if ((fileStat->st_mode & S_IFMT) == S_IFDIR) {
		if (properties->availableBytes) {
			struct statvfs fsStat;
			if (!statvfs(fileName, &fsStat)) {
				char buffer[100];
				unsigned long long size = fsStat.f_bavail * fsStat.f_bsize;
				snprintf(buffer, sizeof(buffer), "%llu", size);
				xmlTextWriterWriteElementString(writer, "d", PROPFIND_AVAILABLE_BYTES, buffer);
				if (properties->usedBytes) {
					size = (fsStat.f_blocks - fsStat.f_bfree) * fsStat.f_bsize;
					snprintf(buffer, sizeof(buffer), "%llu", size);
					xmlTextWriterWriteElementString(writer, "d", PROPFIND_USED_BYTES, buffer);
				}
			}
		}
		if (properties->usedBytes) {
			struct statvfs fsStat;
			if (!statvfs(fileName, &fsStat)) {
				char buffer[100];
				unsigned long long size = (fsStat.f_blocks - fsStat.f_bfree) * fsStat.f_bsize;
				snprintf(buffer, sizeof(buffer), "%llu", size);
				xmlTextWriterWriteElementString(writer, "d", PROPFIND_USED_BYTES, buffer);
			}
		}
		if (properties->windowsHidden) {
			xmlTextWriterWriteElementString(writer, "z", PROPFIND_WINDOWS_ATTRIBUTES,
					displayName[0] == '.' ? "00000012" : "00000010");
		}
	} else {
		if (properties->contentLength) {
			char buffer[100];
			snprintf(buffer, sizeof(buffer), "%lld", (long long) fileStat->st_size);
			xmlTextWriterWriteElementString(writer, "d", PROPFIND_CONTENT_LENGTH, buffer);
		}
		if (properties->contentType) {
			xmlTextWriterWriteElementString(writer, "d", PROPFIND_CONTENT_TYPE, findMimeType(fileName)->type);
		}
		if (properties->windowsHidden) {
			xmlTextWriterWriteElementString(writer, "z", PROPFIND_WINDOWS_ATTRIBUTES,
					displayName[0] == '.' ? "00000022" : "00000020");
		}

	}
	xmlTextWriterEndElement(writer);
	xmlTextWriterWriteElementString(writer, "d", "status", "HTTP/1.1 200 OK");
	xmlTextWriterEndElement(writer);
	xmlTextWriterEndElement(writer);

}

static int respondToPropFind(const char * file, PropertySet * properties, int depth) {
	struct stat fileStat;
	if (stat(file, &fileStat)) {
		int e = errno;
		switch (e) {
		case EACCES:
			stdLogError(e, "PROPFIND access denied %s %s", authenticatedUser, file);
			return respond(RAP_RESPOND_ACCESS_DENIED);
		case ENOENT:
		default:
			stdLogError(e, "PROPFIND not found %s %s", authenticatedUser, file);
			return respond(RAP_RESPOND_NOT_FOUND);
		}
	}

	int pipeEnds[2];
	if (pipe(pipeEnds)) {
		stdLogError(errno, "Could not create pipe to write content");
		return respond(RAP_RESPOND_INTERNAL_ERROR);
	}

	size_t fileNameSize = strlen(file);
	size_t filePathSize = fileNameSize;
	char * filePath = normalizeDirName(file, &filePathSize, (fileStat.st_mode & S_IFMT) == S_IFDIR);

	const char * displayName = &file[fileNameSize - 2];
	while (displayName >= file && *displayName != '/') {
		displayName--;
	}
	displayName++;

	time_t fileTime;
	time(&fileTime);
	Message message = { .mID = RAP_RESPOND_MULTISTATUS, .fd = pipeEnds[PIPE_READ], .paramCount = 2 };
	message.params[RAP_PARAM_RESPONSE_DATE].iov_base = &fileTime;
	message.params[RAP_PARAM_RESPONSE_DATE].iov_len = sizeof(fileTime);
	message.params[RAP_PARAM_RESPONSE_MIME].iov_base = (void *) XML_MIME_TYPE.type;
	message.params[RAP_PARAM_RESPONSE_MIME].iov_len = XML_MIME_TYPE.typeStringSize;
	message.params[RAP_PARAM_RESPONSE_LOCATION].iov_base = filePath;
	message.params[RAP_PARAM_RESPONSE_LOCATION].iov_len = filePathSize + 1;
	ssize_t messageResult = sendMessage(RAP_CONTROL_SOCKET, &message);
	if (messageResult <= 0) {
		freeSafe(filePath);
		close(pipeEnds[PIPE_WRITE]);
		return messageResult;
	}

	// We've set up the pipe and sent read end across so now write the result
	xmlTextWriterPtr writer = xmlNewFdTextWriter(pipeEnds[PIPE_WRITE]);
	DIR * dir;
	xmlTextWriterStartDocument(writer, "1.0", "utf-8", NULL);
	xmlTextWriterStartElementNS(writer, "d", "multistatus", WEBDAV_NAMESPACE);
	xmlTextWriterWriteAttribute(writer, "xmlns:z", MICROSOFT_NAMESPACE);
	writePropFindResponsePart(filePath, displayName, properties, &fileStat, writer);
	if (depth > 1 && (fileStat.st_mode & S_IFMT) == S_IFDIR && (dir = opendir(filePath))) {
		struct dirent * dp;
		char * childFileName = mallocSafe(filePathSize + 257);
		size_t maxSize = 255;
		strcpy(childFileName, filePath);
		while ((dp = readdir(dir)) != NULL) {
			if (dp->d_name[0] != '.' || (dp->d_name[1] != '\0' && dp->d_name[1] != '.')
					|| dp->d_name[2] != '\0') {
				size_t nameSize = strlen(dp->d_name);
				if (nameSize > maxSize) {
					childFileName = reallocSafe(childFileName, filePathSize + nameSize + 2);
					maxSize = nameSize;
				}
				strcpy(childFileName + filePathSize, dp->d_name);
				if (!stat(childFileName, &fileStat)) {
					if ((fileStat.st_mode & S_IFMT) == S_IFDIR) {
						childFileName[filePathSize + nameSize] = '/';
						childFileName[filePathSize + nameSize + 1] = '\0';
					}
					writePropFindResponsePart(childFileName, dp->d_name, properties, &fileStat, writer);
				}
			}
		}
		closedir(dir);
		freeSafe(childFileName);
	}
	xmlTextWriterEndElement(writer);
	xmlFreeTextWriter(writer);
	freeSafe(filePath);
	return messageResult;

}

static ssize_t propfind(Message * requestMessage) {
	if (requestMessage->paramCount != 3) {
		stdLogError(0, "PROPFIND request did not provide correct buffers: %d buffer(s)",
				requestMessage->paramCount);
		close(requestMessage->fd);
		return respond(RAP_RESPOND_INTERNAL_ERROR);
	}

	char * file = messageParamToString(&requestMessage->params[RAP_PARAM_REQUEST_FILE]);
	char * depthString = messageParamToString(&requestMessage->params[RAP_PARAM_REQUEST_DEPTH]);
	if (!depthString) depthString = "1";

	PropertySet properties;
	if (requestMessage->fd == -1) {
		memset(&properties, 1, sizeof(properties));
	} else {
		int ret = respond(RAP_RESPOND_CONTINUE);
		if (ret < 0) {
			return ret;
		}
		if (!parsePropFind(requestMessage->fd, &properties)) {
			return respond(RAP_RESPOND_BAD_CLIENT_REQUEST);
		}
	}

	return respondToPropFind(file, &properties, (strcmp("0", depthString) ? 2 : 1));
}

//////////////////
// End PROPFIND //
//////////////////

///////////////
// PROPPATCH //
///////////////

static ssize_t proppatch(Message * requestMessage) {
	// TODO Need to actually implement something here
	if (requestMessage->fd != -1) {
		respond(RAP_RESPOND_CONTINUE);
		char buffer[BUFFER_SIZE];
		ssize_t bytesRead;
		while ((bytesRead = read(requestMessage->fd, buffer, sizeof(buffer))) > 0) {
			//ssize_t __attribute__ ((unused)) ignored = write(STDERR_FILENO, buffer, bytesRead);
		}

		//char c = '\n';
		//ssize_t __attribute__ ((unused)) ignored = write(STDOUT_FILENO, &c, 1);
		close(requestMessage->fd);
		PropertySet p;
		memset(&p,1, sizeof(p));
		const char * file  = messageParamToString(&requestMessage->params[RAP_PARAM_REQUEST_FILE]);
		return respondToPropFind(file, &p, 1);

	} else {
		return respond(RAP_RESPOND_BAD_CLIENT_REQUEST);
	}
}

///////////////////
// End PROPPATCH //
///////////////////

///////////
// MKCOL //
///////////

static ssize_t mkcol(Message * requestMessage) {
	if (requestMessage->fd != -1) {
		// stdLogError(0, "MKCOL request sent incoming data!");
		close(requestMessage->fd);
	}

	const char * fileName = messageParamToString(&requestMessage->params[RAP_PARAM_REQUEST_FILE]);

	if (mkdir(fileName, NEW_DIR_PREMISSIONS) == -1) {
		int e = errno;
		stdLogError(e, "MKCOL Can not create directory %s", fileName);
		switch (e) {
		case EACCES:
			return writeErrorResponse(RAP_RESPOND_ACCESS_DENIED, strerror(e), NULL, fileName);
		case ENOSPC:
		case EDQUOT:
			return writeErrorResponse(RAP_RESPOND_INSUFFICIENT_STORAGE, strerror(e), NULL, fileName);
		case ENOENT:
		case EPERM:
		case EEXIST:
		case ENOTDIR:
		default:
			return writeErrorResponse(RAP_RESPOND_CONFLICT, strerror(e), NULL, fileName);
		}
	}
	return respond(RAP_RESPOND_CREATED);
}

///////////////
// End MKCOL //
///////////////

//////////
// COPY //
//////////

static int _copyFile(const char * sourceFile, const char * targetFile) {
	return -1;
}

static ssize_t copyFile(Message * requestMessage) {
	// TODO implement this method
	if (requestMessage->fd != -1) {
		// stdLogError(0, "MKCOL request sent incoming data!");
		close(requestMessage->fd);
	}

	return respond(RAP_RESPOND_INTERNAL_ERROR);

}

//////////////
// End COPY //
//////////////

//////////
// MOVE //
//////////

static ssize_t moveFile(Message * requestMessage) {
	if (requestMessage->fd != -1) {
		// stdLogError(0, "MKCOL request sent incoming data!");
		close(requestMessage->fd);
	}

	const char * sourceFile = messageParamToString(&requestMessage->params[RAP_PARAM_REQUEST_FILE]);
	const char * targetFile = messageParamToString(&requestMessage->params[RAP_PARAM_REQUEST_TARGET]);
	if (!targetFile) {
		stdLogError(0, "target not specified in MOVE request");
		return writeErrorResponse(RAP_RESPOND_BAD_CLIENT_REQUEST, "Target not specified", NULL, sourceFile);
	}

	if (rename(sourceFile, targetFile) == -1) {
		if (errno == EXDEV) {
			if (_copyFile(sourceFile, targetFile) != -1) {
				// TODO delete directories.
				if (unlink(sourceFile) == -1) {
					int e = errno;
					stdLogError(e, "Could not move file %s to %s", sourceFile, targetFile);
					return writeErrorResponse(RAP_RESPOND_INTERNAL_ERROR, strerror(e), NULL, sourceFile);
				}
			}
		} else {
			int e = errno;
			stdLogError(e, "Could not move file %s to %s", sourceFile, targetFile);
			switch (e) {
			case EPERM:
			case EACCES:
				return writeErrorResponse(RAP_RESPOND_ACCESS_DENIED, strerror(e), NULL, sourceFile);
			case EDQUOT:
				return writeErrorResponse(RAP_RESPOND_INSUFFICIENT_STORAGE, strerror(e), NULL, sourceFile);
			default:
				return writeErrorResponse(RAP_RESPOND_CONFLICT, strerror(e), NULL, sourceFile);

			}
		}
	}

	return respond(RAP_RESPOND_OK_NO_CONTENT);

}

//////////////
// End MOVE //
//////////////

////////////
// DELETE //
////////////

static ssize_t deleteFile(Message * requestMessage) {
	if (requestMessage->fd != -1) {
		// stdLogError(0, "MKCOL request sent incoming data!");
		close(requestMessage->fd);
	}

	const char * file = messageParamToString(&requestMessage->params[RAP_PARAM_REQUEST_FILE]);
	struct stat fileStat;
	if (stat(file, &fileStat) == -1 || ((fileStat.st_mode & S_IFMT) != S_IFDIR && unlink(file) == -1)
			|| ((fileStat.st_mode & S_IFMT) == S_IFDIR && rmdir(file) == -1)) {

		stdLogError(errno, "Could not delete file %s", file);
		// TODO write error responses
		switch (errno) {
		case EACCES:
		case EPERM:
			return respond(RAP_RESPOND_ACCESS_DENIED);
		case ENOTDIR:
		case ENOENT:
			return respond(RAP_RESPOND_NOT_FOUND);
		default:
			return respond(RAP_RESPOND_INTERNAL_ERROR);
		}
	}

	return respond(RAP_RESPOND_OK_NO_CONTENT);

}

////////////////
// End DELETE //
////////////////

/////////
// PUT //
/////////

static ssize_t writeFile(Message * requestMessage) {
	if (requestMessage->fd == -1) {
		stdLogError(0, "PUT request sent without incoming data!");
		return respond(RAP_RESPOND_INTERNAL_ERROR);
	}

	char * file = messageParamToString(&requestMessage->params[RAP_PARAM_REQUEST_FILE]);
	// TODO change file mode
	int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, NEW_FILE_PERMISSIONS);
	if (fd == -1) {
		int e = errno;
		switch (e) {
		case EACCES:
			stdLogError(e, "PUT access denied %s %s", authenticatedUser, file);
			return respond(RAP_RESPOND_ACCESS_DENIED);
		case ENOENT:
		default:
			stdLogError(e, "PUT not found %s %s", authenticatedUser, file);
			return respond(RAP_RESPOND_CONFLICT);
		}
	}
	int ret = respond(RAP_RESPOND_CONTINUE);
	if (ret < 0) {
		return ret;
	}

	char buffer[BUFFER_SIZE];
	ssize_t bytesRead;

	while ((bytesRead = read(requestMessage->fd, buffer, sizeof(buffer))) > 0) {
		ssize_t bytesWritten = write(fd, buffer, bytesRead);
		if (bytesWritten < bytesRead) {
			stdLogError(errno, "Could wite data to file %s", file);
			close(fd);
			close(requestMessage->fd);
			return respond(RAP_RESPOND_INSUFFICIENT_STORAGE);
		}
	}

	close(fd);
	close(requestMessage->fd);
	return respond(RAP_RESPOND_CREATED);
}

/////////////
// End PUT //
/////////////

/////////
// GET //
/////////

static int compareDirent(const void * a, const void * b) {
	const struct dirent * lhs = *((const struct dirent **) a);
	const struct dirent * rhs = *((const struct dirent **) b);
	int result = strcoll(lhs->d_name, rhs->d_name);
	if (result != 0) {
		return result;
	}
	return strcmp(lhs->d_name, rhs->d_name);
}

static void listDir(const char * file, int dirFd, int writeFd) {
	DIR * dir = fdopendir(dirFd);
	xmlTextWriterPtr writer = xmlNewFdTextWriter(writeFd);

	size_t fileSize = strlen(file);
	char * filePath = normalizeDirName(file, &fileSize, 1);

	size_t entryCount = 0;
	struct dirent ** directoryEntries = NULL;
	struct dirent * dp;
	while ((dp = readdir(dir)) != NULL) {
		int index = entryCount++;
		if (!(index & 0x7F)) {
			directoryEntries = reallocSafe(directoryEntries, sizeof(struct dirent **) * (entryCount + 0x7F));
		}
		directoryEntries[index] = dp;
	}

	qsort(directoryEntries, entryCount, sizeof(*directoryEntries), &compareDirent);

	xmlTextWriterStartElement(writer, "html");
	xmlTextWriterStartElement(writer, "head");
	xmlTextWriterWriteElementString(writer, NULL, "title", filePath);
	xmlTextWriterEndElement(writer);
	xmlTextWriterStartElement(writer, "body");
	xmlTextWriterWriteElementString(writer, NULL, "h1", filePath);
	xmlTextWriterStartElement(writer, "table");
	xmlTextWriterWriteAttribute(writer, "cellpadding", "5");
	xmlTextWriterWriteAttribute(writer, "cellspacing", "5");
	xmlTextWriterWriteAttribute(writer, "border", "1");
	xmlTextWriterWriteElementString(writer, NULL, "th", "Type");
	xmlTextWriterWriteElementString(writer, NULL, "th", "Name");
	xmlTextWriterWriteElementString(writer, NULL, "th", "Size");
	xmlTextWriterWriteElementString(writer, NULL, "th", "Mime Type");
	xmlTextWriterWriteElementString(writer, NULL, "th", "Last Modified");
	for (size_t i = 0; i < entryCount; i++) {
		dp = directoryEntries[i];
		if (dp->d_name[0] != '.') {
			struct stat stat;
			fstatat(dirFd, dp->d_name, &stat, 0);
			char buffer[100];

			xmlTextWriterStartElement(writer, "tr");

			// File or Dir
			xmlTextWriterWriteElementString(writer, NULL, "td", dp->d_type == DT_DIR ? "dir" : "file");

			// File Name
			xmlTextWriterStartElement(writer, "td");
			xmlTextWriterStartElement(writer, "a");
			xmlTextWriterStartAttribute(writer, "href");
			xmlTextWriterWriteURL(writer, filePath);
			xmlTextWriterWriteURL(writer, dp->d_name);
			if (dp->d_type == DT_DIR) xmlTextWriterWriteString(writer, "/");
			xmlTextWriterEndAttribute(writer);
			xmlTextWriterWriteString(writer, dp->d_name);
			if (dp->d_type == DT_DIR) xmlTextWriterWriteString(writer, "/");
			xmlTextWriterEndElement(writer);

			// File Size
			if (dp->d_type == DT_REG) {
				formatFileSize(buffer, sizeof(buffer), stat.st_size);
				xmlTextWriterWriteElementString(writer, NULL, "td", buffer);
			} else {
				xmlTextWriterWriteElementString(writer, NULL, "td", "-");
			}

			// MimeType
			xmlTextWriterWriteElementString(writer, NULL, "td",
					dp->d_type == DT_DIR ? "-" : findMimeType(dp->d_name)->type);

			// Last Modified
			getLocalDate(stat.st_mtime, buffer, sizeof(buffer));
			xmlTextWriterWriteElementString(writer, NULL, "td", buffer);

			xmlTextWriterEndElement(writer);
			xmlTextWriterEndElement(writer);
		}
	}
	xmlTextWriterEndElement(writer);
	xmlTextWriterEndElement(writer);
	xmlTextWriterEndElement(writer);

	freeSafe(filePath);
	xmlFreeTextWriter(writer);
	closedir(dir);
	freeSafe(directoryEntries);
}

static ssize_t readFile(Message * requestMessage) {
	if (requestMessage->fd != -1) {
		stdLogError(0, "GET request sent incoming data!");
		close(requestMessage->fd);
	}

	char * file = messageParamToString(&requestMessage->params[RAP_PARAM_REQUEST_FILE]);
	int fd = open(file, O_RDONLY);
	if (fd == -1) {
		int e = errno;
		switch (e) {
		case EACCES:
			stdLogError(e, "GET access denied %s %s", authenticatedUser, file);
			return respond(RAP_RESPOND_ACCESS_DENIED);
		case ENOENT:
		default:
			stdLogError(e, "GET not found %s %s", authenticatedUser, file);
			return respond(RAP_RESPOND_NOT_FOUND);
		}
	} else {
		struct stat statinfo;
		fstat(fd, &statinfo);

		if ((statinfo.st_mode & S_IFMT) == S_IFDIR) {
			int pipeEnds[2];
			if (pipe(pipeEnds)) {
				stdLogError(errno, "Could not create pipe to write content");
				close(fd);
				return respond(RAP_RESPOND_INTERNAL_ERROR);
			}

			time_t fileTime;
			time(&fileTime);

			Message message = { .mID = RAP_RESPOND_OK, .fd = pipeEnds[PIPE_READ], 3 };
			message.params[RAP_PARAM_RESPONSE_DATE].iov_base = &fileTime;
			message.params[RAP_PARAM_RESPONSE_DATE].iov_len = sizeof(fileTime);
			message.params[RAP_PARAM_RESPONSE_MIME].iov_base = "text/html";
			message.params[RAP_PARAM_RESPONSE_MIME].iov_len = sizeof("text/html");
			message.params[RAP_PARAM_RESPONSE_LOCATION] = requestMessage->params[RAP_PARAM_REQUEST_FILE];
			ssize_t messageResult = sendMessage(RAP_CONTROL_SOCKET, &message);
			if (messageResult <= 0) {
				close(fd);
				close(pipeEnds[PIPE_WRITE]);
				return messageResult;
			}

			listDir(file, fd, pipeEnds[PIPE_WRITE]);
			return messageResult;
		} else {
			Message message = { .mID = RAP_RESPOND_OK, .fd = fd, .paramCount = 3 };
			message.params[RAP_PARAM_RESPONSE_DATE].iov_base = &statinfo.st_mtime;
			message.params[RAP_PARAM_RESPONSE_DATE].iov_len = sizeof(statinfo.st_mtime);
			MimeType * mimeType = findMimeType(file);
			message.params[RAP_PARAM_RESPONSE_MIME].iov_base = (char *) mimeType->type;
			message.params[RAP_PARAM_RESPONSE_MIME].iov_len = mimeType->typeStringSize;
			message.params[RAP_PARAM_RESPONSE_LOCATION] = requestMessage->params[RAP_PARAM_REQUEST_FILE];
			return sendMessage(RAP_CONTROL_SOCKET, &message);
		}
	}
}

/////////////
// End GET //
/////////////

//////////////////
// Authenticate //
//////////////////

static int pamConverse(int n, const struct pam_message **msg, struct pam_response **resp, char * password) {
	struct pam_response * response = mallocSafe(sizeof(struct pam_response));
	response->resp_retcode = 0;
	size_t passSize = strlen(password) + 1;
	response->resp = mallocSafe(passSize);
	memcpy(response->resp, password, passSize);
	*resp = response;
	return PAM_SUCCESS;
}

static void pamCleanup() {
	int pamResult = pam_close_session(pamh, 0);
	pam_end(pamh, pamResult);
}

static int pamAuthenticate(const char * user, const char * password, const char * hostname) {
	static struct pam_conv pamc = { .conv = (int (*)(int num_msg, const struct pam_message **msg,
			struct pam_response **resp, void *appdata_ptr)) &pamConverse };
	pamc.appdata_ptr = (void *) password;
	char ** envList;

	if (pam_start(pamService, user, &pamc, &pamh) != PAM_SUCCESS) {
		stdLogError(0, "Could not start PAM");
		return 0;
	}

	// Authenticate and start session
	int pamResult;
	if ((pamResult = pam_set_item(pamh, PAM_RHOST, hostname)) != PAM_SUCCESS
			|| (pamResult = pam_set_item(pamh, PAM_RUSER, user)) != PAM_SUCCESS || (pamResult =
					pam_authenticate(pamh, PAM_SILENT | PAM_DISALLOW_NULL_AUTHTOK)) != PAM_SUCCESS
			|| (pamResult = pam_acct_mgmt(pamh, PAM_SILENT | PAM_DISALLOW_NULL_AUTHTOK)) != PAM_SUCCESS
			|| (pamResult = pam_setcred(pamh, PAM_ESTABLISH_CRED)) != PAM_SUCCESS || (pamResult =
					pam_open_session(pamh, 0)) != PAM_SUCCESS) {
		pam_end(pamh, pamResult);
		return 0;
	}

	// Get user details
	if ((pamResult = pam_get_item(pamh, PAM_USER, (const void **) &user)) != PAM_SUCCESS || (envList =
			pam_getenvlist(pamh)) == NULL) {

		pamResult = pam_close_session(pamh, 0);
		pam_end(pamh, pamResult);

		return 0;
	}

	// Set up environment and switch user
	clearenv();
	for (char ** pam_env = envList; *pam_env != NULL; ++pam_env) {
		putenv(*pam_env);
		freeSafe(*pam_env);
	}
	freeSafe(envList);

	if (!lockToUser(user)) {
		stdLogError(errno, "Could not set uid or gid");
		pam_close_session(pamh, 0);
		pam_end(pamh, pamResult);
		return 0;
	}

	atexit(&pamCleanup);
	size_t userLen = strlen(user) + 1;
	authenticatedUser = mallocSafe(userLen);
	memcpy((char *) authenticatedUser, user, userLen);

	authenticated = 1;
	return 1;
}

static ssize_t authenticate(Message * message) {
	if (message->fd != -1) {
		stdLogError(0, "authenticate request send incoming data!");
		close(message->fd);
	}

	char * user = messageParamToString(&message->params[RAP_PARAM_AUTH_USER]);
	char * password = messageParamToString(&message->params[RAP_PARAM_AUTH_PASSWORD]);
	char * rhost = messageParamToString(&message->params[RAP_PARAM_AUTH_RHOST]);

	if (pamAuthenticate(user, password, rhost)) {
		//stdLog("Login accepted for %s", user);
		return respond(RAP_RESPOND_OK);
	} else {
		return respond(RAP_RESPOND_AUTH_FAILLED);
	}
}

//////////////////////
// End Authenticate //
//////////////////////

int main(int argCount, char * args[]) {
	setlocale(LC_ALL, "");
	char incomingBuffer[INCOMING_BUFFER_SIZE];
	if (argCount > 1) {
		pamService = args[1];
	} else {
		pamService = "webdav";
	}

	if (argCount > 2) {
		initializeMimeTypes(args[2]);
	} else {
		initializeMimeTypes("/etc/mime.types");
	}

	ssize_t ioResult;
	Message message;
	do {
		ioResult = recvMessage(RAP_CONTROL_SOCKET, &message, incomingBuffer, INCOMING_BUFFER_SIZE);
		if (ioResult <= 0) {
			if (errno == EBADF) {
				stdLogError(0, "Worker threads (%s) must only be created by webdavd", args[0]);
			}
			break;
		}

		if (message.mID == RAP_REQUEST_AUTHENTICATE) {
			ioResult = authenticate(&message);
		} else {
			stdLogError(0, "Invalid request id %d on unauthenticted worker", message.mID);
			ioResult = respond(RAP_RESPOND_INTERNAL_ERROR);
		}

	} while (ioResult > 0 && !authenticated);

	while (ioResult > 0) {
		// Read a message
		ioResult = recvMessage(RAP_CONTROL_SOCKET, &message, incomingBuffer, INCOMING_BUFFER_SIZE);
		if (ioResult <= 0) return ioResult == 0 ? 0 : 1;

		switch (message.mID) {
		case RAP_REQUEST_GET:
			ioResult = readFile(&message);
			break;
		case RAP_REQUEST_PUT:
			ioResult = writeFile(&message);
			break;
		case RAP_REQUEST_MKCOL:
			ioResult = mkcol(&message);
			break;
		case RAP_REQUEST_DELETE:
			ioResult = deleteFile(&message);
			break;
		case RAP_REQUEST_MOVE:
			ioResult = moveFile(&message);
			break;
		case RAP_REQUEST_COPY:
			ioResult = copyFile(&message);
			break;
		case RAP_REQUEST_PROPFIND:
			ioResult = propfind(&message);
			break;
		case RAP_REQUEST_PROPPATCH:
			ioResult = proppatch(&message);
			break;
		case RAP_REQUEST_LOCK:
			ioResult = lockFile(&message);
			break;
		default:
			stdLogError(0, "Invalid request id %d on authenticated worker", message.mID);
			ioResult = respond(RAP_RESPOND_INTERNAL_ERROR);
		}
	}

	return ioResult < 0 ? 1 : 0;
}
