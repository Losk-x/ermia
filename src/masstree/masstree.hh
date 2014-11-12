/* Masstree
 * Eddie Kohler, Yandong Mao, Robert Morris
 * Copyright (c) 2012-2013 President and Fellows of Harvard College
 * Copyright (c) 2012-2013 Massachusetts Institute of Technology
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Masstree LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Masstree LICENSE file; the license in that file
 * is legally binding.
 */
#ifndef MASSTREE_HH
#define MASSTREE_HH
#include "compiler.hh"
#include "str.hh"
#include "ksearch.hh"

#include "../object.h"
#include "../tuple.h"
#include "../dbcore/xid.h"
#include "../macros.h"
#include "../dbcore/sm-alloc.h"

namespace Masstree {
using lcdf::Str;
using lcdf::String;

template <typename T> class value_print;

template <int LW = 15, int IW = LW> struct nodeparams {
    static constexpr int leaf_width = LW;
    static constexpr int internode_width = IW;
    static constexpr bool concurrent = true;
    static constexpr bool prefetch = true;
    static constexpr int bound_method = bound_method_binary;
    static constexpr int debug_level = 0;
    static constexpr bool printable_keys = true;
    typedef uint64_t ikey_type;
};

template <int LW, int IW> constexpr int nodeparams<LW, IW>::leaf_width;
template <int LW, int IW> constexpr int nodeparams<LW, IW>::internode_width;
template <int LW, int IW> constexpr int nodeparams<LW, IW>::debug_level;

template <typename P> class node_base;
template <typename P> class leaf;
template <typename P> class internode;
template <typename P> class leafvalue;
template <typename P> class key;
template <typename P> class basic_table;
template <typename P> class unlocked_tcursor;
template <typename P> class tcursor;

template <typename P>
class basic_table {
  public:
    typedef P param_type;
    typedef node_base<P> node_type;
    typedef leaf<P> leaf_type;
    typedef typename P::value_type value_type;
    typedef typename P::threadinfo_type threadinfo;
    typedef unlocked_tcursor<P> unlocked_cursor_type;
    typedef tcursor<P> cursor_type;

    inline basic_table();

    void initialize(threadinfo& ti);
    void destroy(threadinfo& ti);

    inline node_type* root() const;
    inline node_type* fix_root();

    bool get(Str key, value_type& value, threadinfo& ti) const;

    template <typename F>
    int scan(Str firstkey, bool matchfirst, F& scanner, XID xid, threadinfo& ti) const;
    template <typename F>
    int rscan(Str firstkey, bool matchfirst, F& scanner, XID xid, threadinfo& ti) const;

    template <typename F>
    inline int modify(Str key, F& f, threadinfo& ti);
    template <typename F>
    inline int modify_insert(Str key, F& f, threadinfo& ti);

    inline void print(FILE* f = 0, int indent = 0) const;

	typedef object_vector<value_type> tuple_vector_type; 
	typedef object_vector<node_type*> node_vector_type; 

	inline tuple_vector_type* get_tuple_vector()
	{
		INVARIANT(tuple_vector);
		return tuple_vector;
	}
	inline node_vector_type* get_node_vector()
	{
		INVARIANT(node_vector);
		return node_vector;
	}

	inline oid_type insert_tuple( value_type val )
	{
		INVARIANT( tuple_vector );
		return tuple_vector->insert( val );
	}

	bool update_version( oid_type oid, object* new_desc, XID xid)
	{
		INVARIANT( tuple_vector );
		
		int attempts = 0;
		fat_ptr new_ptr = fat_ptr::make( new_desc, INVALID_SIZE_CODE, fat_ptr::ASI_HOT_FLAG );
	start_over:
        fat_ptr head = tuple_vector->begin(oid);
		object* ptr = (object*)head.offset();
		xid_context *visitor = xid_get_context(xid);
		INVARIANT(visitor->owner == xid);
		dbtuple* version;

		version = reinterpret_cast<dbtuple*>(ptr->payload());
		auto clsn = volatile_read(version->clsn);
		if( clsn.asi_type() == fat_ptr::ASI_XID )
		{
			/* Grab the context for this XID. If we're too slow,
			   the context might be recycled for a different XID,
			   perhaps even *while* we are reading the
			   context. Copy everything we care about and then
			   (last) check the context's XID for a mismatch that
			   would indicate an inconsistent read. If this
			   occurs, just start over---the version we cared
			   about is guaranteed to have a LSN now.
			 */
			//xid tracking
			auto holder_xid = XID::from_ptr(clsn);
			xid_context *holder= xid_get_context(holder_xid);
			INVARIANT(holder);
                        auto end = volatile_read(holder->end);
                        auto state = volatile_read(holder->state);
			auto owner = volatile_read(holder->owner);
			holder = NULL; // use cached values instead!

			// context still valid for this XID?
                        if ( unlikely(owner != holder_xid) ) {
				ASSERT(attempts < 2);
				attempts++;
				goto start_over;
			}
			
			switch (state)
			{
				// if committed and newer data, abort. if not, keep traversing
				case TXN_CMMTD:
					{
						if ( end > visitor->begin )		// to prevent version branch( or lost update)
							return false;
						else
							goto install;
					}

					// aborted data. ignore
				case TXN_ABRTD:
					goto install;

					// dirty data
				case TXN_EMBRYO:
				case TXN_ACTIVE:
					{
						// in-place update case ( multiple updates on the same record  by same transaction)
						if( holder_xid == xid )
							goto install;
						else
							return false;
					}

					// If this TX is committing, we shouldn't install new version!
				case TXN_COMMITTING:
					return false;
				default:
					ALWAYS_ASSERT( false );
			}
		}
		// check dirty writes 
		else 
		{
			// make sure this is valid committed data, or aborted data that is not reclaimed yet.
			// aborted, but not yet reclaimed.
			ASSERT(clsn.asi_type() == fat_ptr::ASI_LOG );
			if ( LSN::from_ptr(clsn) > visitor->begin )
				return false;
			else
				goto install;
		}

install:
		// install a new version
		if(!tuple_vector->put( oid, head, new_ptr))
			return false;
		return true;
	}

	// Sometimes, we don't care about version. We just need the first one!
	inline value_type fetch_latest_version( oid_type oid ) const
	{
		ALWAYS_ASSERT( tuple_vector );
		fat_ptr head = tuple_vector->begin(oid);
		if( head.offset() != 0 )
		{
			object* obj = (object*)head.offset();
			return reinterpret_cast<value_type>( obj->payload() );
		}
		else
			return NULL;
	}

	value_type fetch_version( oid_type oid, XID xid ) const
	{
		INVARIANT( tuple_vector );
		ALWAYS_ASSERT( oid );
		xid_context *visitor= xid_get_context(xid);
		INVARIANT(visitor->owner == xid);

		int attempts = 0;
		object* cur_obj;
	start_over:
		for( fat_ptr ptr = tuple_vector->begin(oid); ptr.offset(); ptr = volatile_read(cur_obj->_next) ) {

            cur_obj = (object*)ptr.offset();
			dbtuple* version = reinterpret_cast<dbtuple*>(cur_obj->payload());
			auto clsn = volatile_read(version->clsn);
			// xid tracking & status check
			if( clsn.asi_type() == fat_ptr::ASI_XID )
			{
				/* Same as above: grab and verify XID context,
				   starting over if it has been recycled
				 */
				auto holder_xid = XID::from_ptr(clsn);
				xid_context *holder = xid_get_context(holder_xid);
				INVARIANT(holder);

				auto state = volatile_read(holder->state);
				auto end = volatile_read(holder->end);
				auto owner = volatile_read(holder->owner);
				holder = NULL; // use cached values instead!
				
				// context still valid for this XID?
				if( unlikely(owner != holder_xid) ) {
					ASSERT(attempts < 2);
					attempts++;
					goto start_over;
				}

				// dirty data made by me is visible!
				if( owner == xid )
					return (value_type)version;
				
				// invalid data
				if( state != TXN_CMMTD)	   // only see committed data.
					continue;

				if( end > visitor->begin  			// committed(but invisible) data, 
						|| end == INVALID_LSN)		// aborted data
					continue;
				return (value_type)version;
			}
			else
			{
				if( LSN::from_ptr(clsn) > visitor->begin ) 	// invisible
					continue;
				return (value_type)version;
			}
		}

		// No Visible records
		return 0;
	}

	inline node_type* fetch_node( oid_type oid ) const
	{
		ALWAYS_ASSERT( node_vector );
		// NOTE: oid 0 indicates absence of the node
		if( oid )
		{
			fat_ptr head = node_vector->begin(oid);
			if( head.offset() != 0 )
			{
				object* obj = (object*)head.offset();
				return (node_type*)(obj->payload());
			}
		}
		return NULL;
	}

	inline void unlink_tuple( oid_type oid, value_type item )
	{
		INVARIANT( tuple_vector );
		INVARIANT( oid );
		return tuple_vector->unlink( oid, item );
	}


  private:
	oid_type root_oid_;
	tuple_vector_type* tuple_vector; 
	node_vector_type* node_vector; 

    template <typename H, typename F>
    int scan(H helper, Str firstkey, bool matchfirst,
	     F& scanner, XID xid, threadinfo& ti) const;

    friend class unlocked_tcursor<P>;
    friend class tcursor<P>;
};

} // namespace Masstree
#endif