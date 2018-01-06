/*
 *    Copyright 2013, 2016 Kai Pastor
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    OpenOrienteering is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with OpenOrienteering.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ocd_types.h"
#include "ocd_types_v8.h"
#include "ocd_types_v9.h"
#include "ocd_types_v10.h"
#include "ocd_types_v11.h"
#include "ocd_types_v12.h"

namespace Ocd
{
	// Verify at compile time that a double is 8 bytes big.
	
	Q_STATIC_ASSERT(sizeof(double) == 8);
	
	// Verify at compile time that data structures are packed, not aligned.
	
	Q_STATIC_ASSERT(sizeof(FileHeaderGeneric) == 8);
	
	Q_STATIC_ASSERT(sizeof(FormatV8::FileHeader) - sizeof(SymbolHeaderV8) == 48);
	
	Q_STATIC_ASSERT(sizeof(FormatV9::FileHeader) == 48);
	
	Q_STATIC_ASSERT(sizeof(FormatV10::FileHeader) == 48);
	
	Q_STATIC_ASSERT(sizeof(FormatV11::FileHeader) == 48);
	
	Q_STATIC_ASSERT(sizeof(FormatV12::FileHeader) == 60);
	
	
	
	const void* getBlockChecked(const QByteArray& byte_array, quint32 pos, quint32 block_size)
	{
		if (pos == 0)
		{
			return nullptr;
		}
		else if (Q_UNLIKELY(pos + block_size - 1 >= static_cast<quint32>(byte_array.size())))
		{
			qWarning("OcdEntityIndexIterator: Next index block is out of bounds");
			return nullptr;
		}
		return byte_array.data() + pos;
	}
	
} // namespace Ocd



// ### OcdEntityIndex implementation ###

template< class F, class T >
typename OcdEntityIndex<F,T>::EntryType& OcdEntityIndex<F,T>::insert(const QByteArray& entity_data, const EntryType& entry)
{
	auto& byte_array = file.byteArray();
	IndexBlock* block;
	auto pos = firstBlock<typename T::IndexEntryType>();
	do
	{
		block = reinterpret_cast<IndexBlock*>(const_cast<void*>(Ocd::getBlockChecked(byte_array, pos, sizeof(IndexBlock))));
		if (Q_UNLIKELY(!block))
		{
			///  \todo Throw exception
			qFatal("OcdEntityIndexIterator: Next index block is out of bounds");
		}
		pos = block->next_block;
	}
	while (pos != 0);
	
	quint16 index = 0;
	while (index < 256 && block->entries[index].pos)
		++index;
	
	if (Q_UNLIKELY(index == 256))
	{
		block->next_block = decltype(block->next_block)(byte_array.size());
		auto new_block = IndexBlock {};
		byte_array.append(reinterpret_cast<const char*>(&new_block), sizeof(IndexBlock));
		block = reinterpret_cast<IndexBlock*>(byte_array.data() + block->next_block);
		index = 0;
	}
	
	auto entity_pos = decltype(block->entries[index].pos)(byte_array.size());
	byte_array.append(entity_data);
	block->entries[index] = entry;
	block->entries[index].pos = entity_pos;
	return block->entries[index];
}



// ### OcdFile implementation ###

template<class F>
OcdFile<F>::OcdFile()
: string_index(*this)
, symbol_index(*this)
, object_index(*this)
{
	byte_array.reserve(1000000);
	byte_array.clear();
	
	{
		FileHeader header = {};
		header.version = F::version;
		if (F::version == 11)
			header.subversion = 3;
		byte_array.append(reinterpret_cast<const char*>(&header), sizeof header);
	}
	
	Q_ASSERT(header());
	
	if (F::version == 8)
	{
		// Note: FileHeaderV8::setup_pos is set by default initialization.
		auto setup = Ocd::SetupV8{};
		byte_array.append(reinterpret_cast<const char*>(&setup), sizeof setup);
	}
	
	{
		header()->first_string_block = static_cast<decltype(header()->first_string_block)>(byte_array.size());
		using StringIndexBlock = Ocd::IndexBlock<Ocd::ParameterStringIndexEntry>;
		auto first_string_block = StringIndexBlock{};
		byte_array.append(reinterpret_cast<const char*>(&first_string_block), sizeof(StringIndexBlock));
		// Free stack from first_string_block
	}
	
	{
		header()->first_symbol_block = static_cast<decltype(header()->first_symbol_block)>(byte_array.size());
		using SymbolIndexBlock = Ocd::IndexBlock<typename F::BaseSymbol::IndexEntryType>;
		auto first_symbol_block = SymbolIndexBlock{};
		byte_array.append(reinterpret_cast<const char*>(&first_symbol_block), sizeof(SymbolIndexBlock));
		// Free stack from first_symbol_block
	}
	
	{
		header()->first_object_block = static_cast<decltype(header()->first_object_block)>(byte_array.size());
		using ObjectIndexBlock = Ocd::IndexBlock<typename F::Object::IndexEntryType>;
		auto first_object_block = ObjectIndexBlock{};
		byte_array.append(reinterpret_cast<const char*>(&first_object_block), sizeof(ObjectIndexBlock));
		// Free stack from first_object_block
	}
}



// Explicit instantiations

OCD_EXPLICIT_INSTANTIATION(template, Ocd::FormatV8)
OCD_EXPLICIT_INSTANTIATION(template, Ocd::FormatV9)
Q_STATIC_ASSERT((std::is_same<Ocd::FormatV10, Ocd::FormatV9>::value));
OCD_EXPLICIT_INSTANTIATION(template, Ocd::FormatV11)
OCD_EXPLICIT_INSTANTIATION(template, Ocd::FormatV12)
