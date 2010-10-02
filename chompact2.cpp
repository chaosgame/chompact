// Copyright (c) 2010, Nathan Lawrence <ch@osga.me>
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of the <organization> nor the
//       names of its contributors may be used to endorse or promote products
//       derived from this software without specific prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <iostream>
#include <stdint.h>
#include <vector>

#ifndef NDEBUG
#include <typeinfo>
#endif

#define DynamicClass(C) \
	template<typename IdType> class C##Impl; \
	struct C { \
		enum IdType { C##IdSentinal }; \
		typedef C##Impl< IdType > DynImpl; \
	}; \
	template<typename IdType> class C##Impl : public DynamicBase
#define DynamicMember(C) Member< IdType, C >
#define extends ,
#define Dyn(C) DynamicWrapper< C >
#define DynId(C) C::IdType
#define DynInfo(C) DynamicObjectInfo<DynId(Class)>::info

class DynamicBase;
template<typename Class> class DynamicWrapper;

//! TODO
template<typename ClassId>
class ObjectInfo
{
public:
	ObjectInfo()
		: finalized(false)
	{
	}

	void populate(uintptr_t base)
	{
		// Create an object of type Class.  This instance is going to populate
		// ObjectInfo<Class> with pointers to all of the reference members of Class.
		// Once we have a list of all of the reference members in Class, we normalize
		// them to be integer offsets from the beginning of a Class object rather than
		// direct pointers to a single instance.
		for(size_t i = 0; i < children.size(); ++i)
			children[i] -= base;
#ifndef NDEBUG
		std::cout << "resolving: " << typeid(*this).name() << std::endl;
		for(size_t i = 0; i < children.size(); ++i)
			std::cout << "child: " << children[i] << std::endl;
#endif

		// Finalize the object so that Member<Class,*> doesn't call ObjectInfo::append
		// from the constructor anymore
		finalized = true;
	}

	bool finalized;
	std::vector<uintptr_t> children;
};

//! TODO
template<typename ClassId>
struct DynamicObjectInfo
{
	static ObjectInfo<ClassId> info;
};
template<typename ClassId> ObjectInfo<ClassId> DynamicObjectInfo<ClassId>::info;

//! Member<> is a wrapper around the data members of a class used to automatically
//! generate the list of objects that need to be marked for each type.
template<typename ClassId>
class MemberBase
{
public:
	MemberBase()
		: m_ptr(0)
	{
		if (DynamicObjectInfo<ClassId>::info.finalized)
			return;

		DynamicObjectInfo<ClassId>::info.children.push_back(reinterpret_cast<uintptr_t>(this));
	}

private:
	void* m_ptr;
};

//! Member is a wrapper around a pointer member of a c++ class.  The first template parameter
//! is the type of the class that the member belongs to, while the second template parameter
//! the type of the pointer member.  For example, in a class Dictionary, a pointer member to
//! a type String would be Member<Dictionary, String>
template<typename ClassId, typename Property>
class Member : public MemberBase<ClassId>
{
public:
	Member() {}
	Member(DynamicWrapper<Property>*);

	Property& operator*() const { return *reinterpret_cast<Property*>(MemberBase<ClassId>::m_ptr); }
	Property* operator->() const { return reinterpret_cast<Property*>(MemberBase<ClassId>::m_ptr); }

	operator bool() const { return MemberBase<ClassId>::m_ptr; }

	/*Member& operator=(DynamicWrapper<Property>* collected)
	{
		MemberBase<ClassId>::m_ptr = collected;
		return *this;
	}*/

	template<typename T>
	Member& operator=(const Member<T, Property>& handle)
	{
		MemberBase<ClassId>::m_ptr = handle.MemberBase<T>::m_ptr;
		return *this;
	}
};

//! TODO
class DynamicBase
{
public:
	virtual std::pair<DynamicBase**,DynamicBase**> children() = 0;
};

//! TODO
template<typename Class>
class DynamicWrapper : public Class::DynImpl
{
public:
	DynamicWrapper()
	{
		DynamicObjectInfo<typename Class::IdType>::info.populate(reinterpret_cast<uintptr_t>(this));
	}

	std::pair<DynamicBase**,DynamicBase**> children()
	{
		return std::make_pair(
		          reinterpret_cast<DynamicBase**>(&DynamicObjectInfo<typename Class::IdType>::info.children.front()),
		          reinterpret_cast<DynamicBase**>(&DynamicObjectInfo<typename Class::IdType>::info.children.back()));
	}
};

DynamicClass(List)
{
public:
	int data;
	DynamicMember(List) next;
};


int main()
{
	Dyn(List)* head = new Dyn(List);
}

