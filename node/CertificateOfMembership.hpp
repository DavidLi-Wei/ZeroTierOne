/*
 * Copyright (c)2013-2020 ZeroTier, Inc.
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file in the project's root directory.
 *
 * Change Date: 2024-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2.0 of the Apache License.
 */
/****/

#ifndef ZT_CERTIFICATEOFMEMBERSHIP_HPP
#define ZT_CERTIFICATEOFMEMBERSHIP_HPP

#include <cstdint>
#include <cstring>

#include <string>
#include <stdexcept>
#include <algorithm>

#include "Constants.hpp"
#include "Credential.hpp"
#include "Address.hpp"
#include "C25519.hpp"
#include "Identity.hpp"
#include "Utils.hpp"

/**
 * Maximum number of qualifiers allowed in a COM (absolute max: 65535)
 */
#define ZT_NETWORK_COM_MAX_QUALIFIERS 8

#define ZT_CERTIFICATEOFMEMBERSHIP_MARSHAL_SIZE_MAX (1 + 2 + (24 * ZT_NETWORK_COM_MAX_QUALIFIERS) + 5 + ZT_SIGNATURE_BUFFER_SIZE)

namespace ZeroTier {

class RuntimeEnvironment;

/**
 * Certificate of network membership
 *
 * The COM contains a sorted set of three-element tuples called qualifiers.
 * These contain an id, a value, and a maximum delta.
 *
 * The ID is arbitrary and should be assigned using a scheme that makes
 * every ID globally unique. IDs beneath 65536 are reserved for global
 * assignment by ZeroTier Networks.
 *
 * The value's meaning is ID-specific and isn't important here. What's
 * important is the value and the third member of the tuple: the maximum
 * delta. The maximum delta is the maximum difference permitted between
 * values for a given ID between certificates for the two certificates to
 * themselves agree.
 *
 * Network membership is checked by checking whether a peer's certificate
 * agrees with your own. The timestamp provides the fundamental criterion--
 * each member of a private network must constantly obtain new certificates
 * often enough to stay within the max delta for this qualifier. But other
 * criteria could be added in the future for very special behaviors, things
 * like latitude and longitude for instance.
 *
 * This is a memcpy()'able structure and is safe (in a crash sense) to modify
 * without locks.
 */
class CertificateOfMembership : public Credential
{
	friend class Credential;

public:
	static ZT_ALWAYS_INLINE ZT_CredentialType credentialType() { return ZT_CREDENTIAL_TYPE_COM; }

	/**
	 * Reserved qualifier IDs
	 *
	 * IDs below 1024 are reserved for use as standard IDs. Others are available
	 * for user-defined use.
	 *
	 * Addition of new required fields requires that code in hasRequiredFields
	 * be updated as well.
	 */
	enum ReservedId
	{
		/**
		 * Timestamp of certificate
		 */
		COM_RESERVED_ID_TIMESTAMP = 0,

		/**
		 * Network ID for which certificate was issued
		 */
		COM_RESERVED_ID_NETWORK_ID = 1,

		/**
		 * ZeroTier address to whom certificate was issued
		 */
		COM_RESERVED_ID_ISSUED_TO = 2
	};

	/**
	 * Create an empty certificate of membership
	 */
	ZT_ALWAYS_INLINE CertificateOfMembership() :
		_qualifierCount(0),
		_signatureLength(0) {}

	/**
	 * Create from required fields common to all networks
	 *
	 * @param timestamp Timestamp of certificate
	 * @param timestampMaxDelta Maximum variation between timestamps on this net
	 * @param nwid Network ID
	 * @param issuedTo Certificate recipient
	 */
	CertificateOfMembership(uint64_t timestamp,uint64_t timestampMaxDelta,uint64_t nwid,const Address &issuedTo);

	/**
	 * @return True if there's something here
	 */
	ZT_ALWAYS_INLINE operator bool() const { return (_qualifierCount != 0); }

	/**
	 * @return Credential ID, always 0 for COMs
	 */
	ZT_ALWAYS_INLINE uint32_t id() const { return 0; }

	/**
	 * @return Timestamp for this cert and maximum delta for timestamp
	 */
	ZT_ALWAYS_INLINE int64_t timestamp() const
	{
		for(unsigned int i=0;i<_qualifierCount;++i) {
			if (_qualifiers[i].id == COM_RESERVED_ID_TIMESTAMP)
				return (int64_t)_qualifiers[i].value;
		}
		return 0;
	}

	/**
	 * @return Address to which this cert was issued
	 */
	ZT_ALWAYS_INLINE Address issuedTo() const
	{
		for(unsigned int i=0;i<_qualifierCount;++i) {
			if (_qualifiers[i].id == COM_RESERVED_ID_ISSUED_TO)
				return Address(_qualifiers[i].value);
		}
		return Address();
	}

	/**
	 * @return Network ID for which this cert was issued
	 */
	ZT_ALWAYS_INLINE uint64_t networkId() const
	{
		for(unsigned int i=0;i<_qualifierCount;++i) {
			if (_qualifiers[i].id == COM_RESERVED_ID_NETWORK_ID)
				return _qualifiers[i].value;
		}
		return 0ULL;
	}

	/**
	 * Add or update a qualifier in this certificate
	 *
	 * Any signature is invalidated and signedBy is set to null.
	 *
	 * @param id Qualifier ID
	 * @param value Qualifier value
	 * @param maxDelta Qualifier maximum allowed difference (absolute value of difference)
	 */
	void setQualifier(uint64_t id,uint64_t value,uint64_t maxDelta);

	ZT_ALWAYS_INLINE void setQualifier(ReservedId id,uint64_t value,uint64_t maxDelta) { setQualifier((uint64_t)id,value,maxDelta); }

	/**
	 * Compare two certificates for parameter agreement
	 *
	 * This compares this certificate with the other and returns true if all
	 * parameters in this cert are present in the other and if they agree to
	 * within this cert's max delta value for each given parameter.
	 *
	 * Tuples present in other but not in this cert are ignored, but any
	 * tuples present in this cert but not in other result in 'false'.
	 *
	 * @param other Cert to compare with
	 * @return True if certs agree and 'other' may be communicated with
	 */
	ZT_ALWAYS_INLINE bool agreesWith(const CertificateOfMembership &other) const
	{
		unsigned int myidx = 0;
		unsigned int otheridx = 0;

		if ((_qualifierCount == 0)||(other._qualifierCount == 0))
			return false;

		while (myidx < _qualifierCount) {
			// Fail if we're at the end of other, since this means the field is
			// missing.
			if (otheridx >= other._qualifierCount)
				return false;

			// Seek to corresponding tuple in other, ignoring tuples that
			// we may not have. If we run off the end of other, the tuple is
			// missing. This works because tuples are sorted by ID.
			while (other._qualifiers[otheridx].id != _qualifiers[myidx].id) {
				++otheridx;
				if (otheridx >= other._qualifierCount)
					return false;
			}

			// Compare to determine if the absolute value of the difference
			// between these two parameters is within our maxDelta.
			const uint64_t a = _qualifiers[myidx].value;
			const uint64_t b = other._qualifiers[myidx].value;
			if (((a >= b) ? (a - b) : (b - a)) > _qualifiers[myidx].maxDelta)
				return false;

			++myidx;
		}

		return true;
	}

	/**
	 * Sign this certificate
	 *
	 * @param with Identity to sign with, must include private key
	 * @return True if signature was successful
	 */
	bool sign(const Identity &with);

	/**
	 * Verify this COM and its signature
	 *
	 * @param RR Runtime environment for looking up peers
	 * @param tPtr Thread pointer to be handed through to any callbacks called as a result of this call
	 */
	ZT_ALWAYS_INLINE Credential::VerifyResult verify(const RuntimeEnvironment *RR,void *tPtr) const { return _verify(RR,tPtr,*this); }

	/**
	 * @return Address that signed this certificate or null address if none
	 */
	ZT_ALWAYS_INLINE const Address &signedBy() const { return _signedBy; }

	static ZT_ALWAYS_INLINE int marshalSizeMax() { return ZT_CERTIFICATEOFMEMBERSHIP_MARSHAL_SIZE_MAX; }
	int marshal(uint8_t data[ZT_CERTIFICATEOFMEMBERSHIP_MARSHAL_SIZE_MAX]) const;
	int unmarshal(const uint8_t *data,int len);

	bool operator==(const CertificateOfMembership &c) const;
	ZT_ALWAYS_INLINE bool operator!=(const CertificateOfMembership &c) const { return (!(*this == c)); }

private:
	struct _Qualifier
	{
		ZT_ALWAYS_INLINE _Qualifier() : id(0),value(0),maxDelta(0) {}
		uint64_t id;
		uint64_t value;
		uint64_t maxDelta;
		ZT_ALWAYS_INLINE bool operator<(const _Qualifier &q) const { return (id < q.id); } // sort order
	};

	Address _signedBy;
	_Qualifier _qualifiers[ZT_NETWORK_COM_MAX_QUALIFIERS];
	unsigned int _qualifierCount;
	unsigned int _signatureLength;
	uint8_t _signature[ZT_SIGNATURE_BUFFER_SIZE];
};

} // namespace ZeroTier

#endif
