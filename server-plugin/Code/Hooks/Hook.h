/*
	Copyright 2012 - Le Padellec Sylvain

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at
	   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#ifndef HOOK_H
#define HOOK_H

#include <limits>
#include <iostream>
#include <algorithm>

#ifdef WIN32
#	include "Misc/include_windows_headers.h"
#	define HOOKFN_EXT __thiscall
#	define HOOKFN_INT __fastcall
#else
#	include <sys/mman.h>
#	include <unistd.h>
#	define HOOKFN_EXT
#	define HOOKFN_INT __attribute__((cdecl))
#endif

#include "SdkPreprocessors.h"
#include "Interfaces/iserverunknown.h"

#include "Preprocessors.h"
#include "Players/NczPlayerManager.h"
#include "Misc/HeapMemoryManager.h"
#include "Systems/Logger.h"
#include "Misc/Helpers.h"

/*
	Used to replace a pointer in one virtual table.
	Returns the old function pointer or 0 in case of error.
*/

void MoveVirtualFunction ( DWORD const * const from, DWORD * const to );

class CBaseEntity;

/*
	HookInfo will store data about one hooked instance.
	It is often used in a list when we need to hook thousands of classes that don't really have the same virtual table.
*/

struct HookInfo
{
	DWORD oldFn; // Old function address that we replace in the vtable
	void* origEnt; // Address of the class used to determine virtual table base
	DWORD* pInterface; // Virtual table base
	DWORD* vf_entry; // Pointer to the entry in the virtual table
	DWORD newFn; // New function address that will be at *vf_entry after hook

	HookInfo ()
	{
		memset ( this, 0, sizeof ( HookInfo ) );
	}
	HookInfo ( const HookInfo& other )
	{
		memcpy ( this, &other, sizeof ( HookInfo ) );
	}
	HookInfo& operator=( const HookInfo& other )
	{
		memcpy ( this, &other, sizeof ( HookInfo ) );
		return *this;
	}
	HookInfo ( void* class_ptr, int vfid, DWORD new_fn ) :
		oldFn ( 0 ),
		origEnt( class_ptr ),
		pInterface ( ( DWORD* )*( DWORD* ) origEnt ),
		vf_entry ( &( pInterface[ vfid ] ) ),
		newFn ( new_fn )
	{}
	HookInfo ( void* class_ptr, int vfid ) :
		origEnt ( class_ptr ),
		pInterface ( ( DWORD* )*( DWORD* ) origEnt ),
		vf_entry ( &( pInterface[ vfid ] ) )
	{}

	bool operator== ( const HookInfo& other ) const
	{
		return ( vf_entry == other.vf_entry );
	}
};

typedef CUtlVector<HookInfo> hooked_list_t;

extern int HookCompare ( HookInfo const * a, HookInfo const * b );

template <class CallerTicket>
class HookGuard :
	public Singleton< HookGuard < CallerTicket > >
{
private:
	typedef Singleton< HookGuard < CallerTicket > > singleton_class;

	hooked_list_t m_list;

public:
	HookGuard () : singleton_class ()
	{}
	virtual ~HookGuard () final
	{};

	void VirtualTableHook ( HookInfo& info, bool force = false )
	{
		if( m_list.HasElement ( info ) && !force ) return;

		//info.oldFn = 0;
#ifdef WIN32
		DWORD dwOld;
		if( !VirtualProtect ( info.vf_entry, 2 * sizeof ( DWORD* ), PAGE_EXECUTE_READWRITE, &dwOld ) )
		{
			return;
		}
#else // LINUX
		uint32_t psize ( sysconf ( _SC_PAGESIZE ) );
		void *p ( ( void * ) ( ( DWORD ) ( info.vf_entry ) & ~( psize - 1 ) ) );
		if( mprotect ( p, ( ( 2 * sizeof ( void * ) ) + ( ( DWORD ) ( info.vf_entry ) & ( psize - 1 ) ) ), PROT_READ | PROT_WRITE | PROT_EXEC ) < 0 )
		{
			return;
		}
#endif // WIN32

		if( info.oldFn && info.oldFn != *info.vf_entry )
			Logger::GetInstance ()->Msg<MSG_WARNING> ( "VirtualTableHook : Unexpected virtual table value in VirtualTableHook. Another plugin might be in conflict." );
		if( info.newFn == *info.vf_entry )
		{

			if( !m_list.HasElement ( info ) )
			{
				Logger::GetInstance ()->Msg<MSG_WARNING> ( "VirtualTableHook : Virtual function pointer was the same but not registered as hooked ..." );
				m_list.AddToTail ( info );
				m_list.Sort ( HookCompare );
			}
			return;
		}

		info.oldFn = *info.vf_entry;
		*info.vf_entry = info.newFn;

#ifdef WIN32
		VirtualProtect ( info.vf_entry, 2 * sizeof ( DWORD* ), dwOld, &dwOld );
#else // LINUX
		mprotect ( p, ( ( 2 * sizeof ( void * ) ) + ( ( DWORD ) ( info.vf_entry ) & ( psize - 1 ) ) ), PROT_READ | PROT_EXEC );
#endif // WIN32
		DebugMessage ( Helpers::format ( "VirtualTableHook : function 0x%X at 0x%X replaced by 0x%X.", info.oldFn, info.vf_entry, info.newFn ) );

		if( !m_list.HasElement ( info ) )
		{
			m_list.AddToTail ( info );
			m_list.Sort ( HookCompare );
		}
	}

	// Find by virtual table entry address
	DWORD RT_GetOldFunction ( void* class_ptr, int vfid ) const
	{
		int it ( m_list.Find ( HookInfo ( class_ptr, vfid ) ) );
		if( it != -1 )
		{
			return m_list[ it ].oldFn;
		}
		else
		{
			return 0;
		}
	}

	// Only find by virtual table base (Remove need to call ConfigManager), class_ptr is converted to virtual table base
	DWORD RT_GetOldFunctionByVirtualTable ( void* class_ptr ) const
	{
		DWORD* vt ( ( ( DWORD* )*( DWORD* ) class_ptr ) );
		for( hooked_list_t::const_iterator it ( m_list.begin () ); it != m_list.end (); ++it )
		{
			if( it->pInterface == vt )
			{
				return it->oldFn;
			}
		}
		return 0;
	}

	// Only find by virtual table base (Remove need to call ConfigManager), class_ptr is the original instance
	DWORD RT_GetOldFunctionByInstance ( void* class_ptr ) const
	{
		for( hooked_list_t::const_iterator it ( m_list.begin () ); it != m_list.end (); ++it )
		{
			if( it->origEnt == class_ptr )
			{
				return it->oldFn;
			}
		}
		return 0;
	}

	void UnhookAll ()
	{
		for( hooked_list_t::iterator it ( m_list.begin () ); it != m_list.end (); ++it )
		{
			std::swap ( it->newFn, it->oldFn );
			VirtualTableHook ( *it, true ); // unhook
		}
		m_list.RemoveAll ();
	}
};

/*
	Some testers need to receive callbacks in a specific order against other testers.
	This is used to save some operations when a tester detected something.
*/
template <class C>
struct SortedListener
{
	mutable C * listener;
	size_t priority;
	SlotStatus filter;
};

template <class C>
class HookListenersList
{
	typedef SortedListener<C> inner_type;
public:
	struct alignas(8) elem_t :
		HeapMemoryManager::OverrideNew<8>
	{
		elem_t ()
		{
			m_next = nullptr;
		}

		inner_type m_value;
		elem_t* m_next;
	};

private:

	elem_t* m_first;

public:

	HookListenersList ()
	{
		m_first = nullptr;
	}
	~HookListenersList ()
	{
		while( m_first != nullptr )
		{
			Remove ( m_first->m_value.listener );
		}
	}

	elem_t* GetFirst () const
	{
		return m_first;
	}

	/*
		Add a listener sorted by priority.
	*/
	elem_t* Add ( C const * const listener, size_t const priority = 0, SlotStatus const filter = SlotStatus::PLAYER_IN_TESTS )
	{
		if( m_first == nullptr )
		{
			m_first = new elem_t ();
			__assume ( m_first != nullptr );
			m_first->m_value.listener = const_cast< C * const >( listener );
			m_first->m_value.priority = priority;
			m_first->m_value.filter = filter;
			return m_first;
		}
		else
		{
			elem_t* iterator = m_first;
			elem_t* prev = nullptr;
			do
			{
				if( priority <= iterator->m_value.priority )
				{
					// Insert here

					if( prev == nullptr ) // iterator == m_first
					{
						elem_t* const old_first = m_first;
						m_first = new elem_t ();
						__assume ( m_first != nullptr );
						m_first->m_next = old_first;
						m_first->m_value.listener = const_cast< C * const >( listener );
						m_first->m_value.priority = priority;
						m_first->m_value.filter = filter;
						return m_first;
					}
					else
					{
						prev->m_next = new elem_t ();
						prev->m_next->m_next = iterator;
						prev->m_next->m_value.listener = const_cast< C * const >( listener );
						prev->m_next->m_value.priority = priority;
						prev->m_next->m_value.filter = filter;
						return prev->m_next;
					}
				}

				prev = iterator;
				iterator = iterator->m_next;
			}
			while( iterator != nullptr );

			prev->m_next = new elem_t ();
			__assume ( prev->m_next != nullptr );
			prev->m_next->m_value.listener = const_cast< C * const >( listener );
			prev->m_next->m_value.priority = priority;
			prev->m_next->m_value.filter = filter;
			return prev->m_next;
		}
	}

	/*
		Find this listener and remove it from list
	*/
	void Remove ( C const * const listener )
	{
		elem_t* iterator = m_first;
		elem_t* prev = nullptr;
		while( iterator != nullptr )
		{
			if( iterator->m_value.listener == listener )
			{
				elem_t* to_remove = iterator;
				if( prev == nullptr )
				{
					m_first = iterator->m_next;
				}
				else
				{
					prev->m_next = iterator->m_next;
				}
				iterator = iterator->m_next;
				delete to_remove;
				return;
			}
			prev = iterator;
			iterator = iterator->m_next;
		}
	}

	/*
		Find by listener
	*/
	elem_t* const FindByListener ( C const * const listener, C const * const exclude_me = nullptr ) const
	{
		elem_t* iterator = m_first;
		while( iterator != nullptr )
		{
			if( iterator->m_value.listener != exclude_me )
			{
				if( iterator->m_value.listener == listener )
				{
					return iterator;
				}
			}
			iterator = iterator->m_next;
		}
		return nullptr;
	}
};

#endif
