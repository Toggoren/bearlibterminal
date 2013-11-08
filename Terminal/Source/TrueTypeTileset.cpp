/*
 * FreeTypeTileset.cpp
 *
 *  Created on: Nov 7, 2013
 *      Author: cfyz
 */

#include "TrueTypeTileset.hpp"
#include "Geometry.hpp"
#include "Utility.hpp"
#include "Log.hpp"
#include <cmath>
#include <freetype/ftlcdfil.h>
#include <freetype/ftglyph.h>

namespace BearLibTerminal
{
	TrueTypeTileset::TrueTypeTileset(TileContainer& container, OptionGroup& group):
		StronglyTypedReloadableTileset(container),
		m_base_code(0),
		m_alignment(Tile::Alignment::Unknown),
		m_font_library(nullptr),
		m_font_face(nullptr),
		m_render_mode(FT_RENDER_MODE_NORMAL),
		m_monospace(false)
	{
		if (group.name != L"font" && !try_parse(group.name, m_base_code))
		{
			throw std::runtime_error("TrueTypeTileset: failed to parse base code");
		}

		if (!group.attributes.count(L"name") || group.attributes[L"name"].empty())
		{
			throw std::runtime_error("TrueTypeTileset: missing or empty 'name' attribute");
		}

		if (group.attributes.count(L"bbox") && !try_parse(group.attributes[L"bbox"], m_bbox_size))
		{
			throw std::runtime_error("TrueTypeTileset: failed to parse 'bbox' attribute");
		}

		if (m_bbox_size.width < 1 || m_bbox_size.height < 1)
		{
			m_bbox_size = Size(1, 1);
		}

		if (group.attributes.count(L"codepage"))
		{
			// TODO: check for error
			m_codepage = GetUnibyteEncoding(group.attributes[L"codepage"]);
		}
		else
		{
			m_codepage = GetUnibyteEncoding(m_base_code == 0? L"utf8": L"ascii");
		}

		if (group.attributes.count(L"align") && !try_parse(group.attributes[L"align"], m_alignment))
		{
			throw std::runtime_error("TrueTypeTileset: failed to parse 'alignemnt' attribute");
		}

		if (group.attributes.count(L"mode"))
		{
			std::wstring mode_str = group.attributes[L"mode"];
			if (mode_str == L"normal")
			{
				m_render_mode = FT_RENDER_MODE_NORMAL;
			}
			else if (mode_str == L"monochrome")
			{
				m_render_mode = FT_RENDER_MODE_MONO;
			}
			else if (mode_str == L"lcd")
			{
				m_render_mode = FT_RENDER_MODE_LCD;
			}
			else
			{
				throw std::runtime_error("TrueTypeTileset: failed to parse 'mode' attribute");
			}
		}

		if (group.attributes.count(L"size"))
		{
			std::wstring size_str = group.attributes[L"size"];
			if (size_str.find(L"x") != std::wstring::npos)
			{
				if (!try_parse(size_str, m_tile_size))
				{
					throw std::runtime_error("TrueTypeTileset: failed to parse 'size' attribute");
				}
			}
			else
			{
				if (!try_parse(size_str, m_tile_size.height))
				{
					throw std::runtime_error("TrueTypeTileset: failed to parse 'size' attribute");
				}
			}

			if (m_tile_size.width < 0 || m_tile_size.height < 1)
			{
				throw std::runtime_error("TrueTypeTileset: size %size is out of acceptable range"); // TODO: formatting
			}
		}
		else
		{
			throw std::runtime_error("TrueTypeTileset: missing 'size' attribute");
		}

		// Trying to initialize FreeType

		if (FT_Init_FreeType(&m_font_library))
		{
			throw std::runtime_error("TrueTypeTileset: can't initialize Freetype");
		}

		std::string filename_u8 = UTF8->Convert(group.attributes[L"name"]);
		if (FT_New_Face(m_font_library, filename_u8.c_str(), 0, &m_font_face))
		{
			throw std::runtime_error("TrueTypeTileset: can't load font from file");
		}

		int hres = 64;
		FT_Matrix matrix =
		{
			(int)((1.0/hres) * 0x10000L),
			(int)((0.0)      * 0x10000L),
			(int)((0.0)      * 0x10000L),
			(int)((1.0)      * 0x10000L)
		};
		FT_Set_Transform(m_font_face, &matrix, NULL);

		auto get_metrics = [&](uint16_t code) -> FT_Glyph_Metrics
		{
			if (FT_Load_Glyph(m_font_face, FT_Get_Char_Index(m_font_face, code), 0))
			{
				throw std::runtime_error("TrueTypeTileset: glyph loading error");
			}
			return m_font_face->glyph->metrics;
		};

		if (m_tile_size.width == 0)
		{
			// Only height was specified, e. g. size=12

			if (FT_Set_Char_Size(m_font_face, 0, (uint32_t)(m_tile_size.height*64), 96*hres, 96))
			{
				throw std::runtime_error("TrueTypeTileset: can't setup font size");
			}

			int dot_width = (int)std::ceil(get_metrics('.').horiAdvance/64.0f/64.0f);
			int at_width = (int)std::ceil(get_metrics('@').horiAdvance/64.0f/64.0f);
			m_monospace = dot_width == at_width;

			int height = m_font_face->size->metrics.height >> 6;
			int width = at_width;

			m_tile_size = Size(width, height);

			LOG(Trace, "Font tile size is " << m_tile_size << ", font is " << (m_monospace? "monospace": "not monospace"));
		}
		else
		{
			// Glyph bbox was specified, e. g. size=8x12
			// Must guess required character size

			auto get_size = [&](float height) -> Size
			{
				if (FT_Set_Char_Size(m_font_face, 0, (uint32_t)(height*64), 96*hres, 96))
				{
					throw std::runtime_error("TrueTypeTileset: can't setup font size");
				}

				int w = (int)std::ceil(get_metrics('@').horiAdvance/64.0f/64.0f);
				int h = m_font_face->size->metrics.height >> 6;

				return Size(w, h);
			};

			float left = 1.0f;
			float right = 96.0f;
			float guessed = 10.0f;

			while (true)
			{
				Size size = get_size(guessed);
				if (size.width > m_tile_size.width || size.height > m_tile_size.height)
				{
					// This is too big by at least one dimension.
					float temp = guessed;
					right = guessed;
					guessed = (left + right) / 2.0f;
					LOG(Trace, "height " << temp << " was to big (" << size << " / " << m_tile_size << "), will try " << guessed << "; [" << left << ", " << right << "]");
				}
				else if (size.width <= m_tile_size.width && size.height <= m_tile_size.height)
				{
					// Smaller than or equal to requested size.
					if (right - guessed < 1.0f)
					{
						LOG(Trace, "height " << guessed << " is smaller or equal (" << size << " / " << m_tile_size << "), but right border " << right << " is too close, stopping");
						break;
					}
					else
					{
						float temp = guessed;
						left = guessed;
						guessed = (left + right) / 2.0f;
						LOG(Trace, "height " << temp << " is smaller or equal (" << size << " / " << m_tile_size << "), will try " << guessed << "; [" << left << ", " << right << "]");
					}
				}
			}

			int dot_width = (int)std::ceil(get_metrics('.').horiAdvance/64.0f/64.0f);
			int at_width = (int)std::ceil(get_metrics('@').horiAdvance/64.0f/64.0f);
			m_monospace = dot_width == at_width;

			int height = m_font_face->size->metrics.height >> 6;
			int width = at_width;

			m_tile_size = Size(width, height);

			LOG(Trace, "Font tile size is " << m_tile_size << ", font is " << (m_monospace? "monospace": "not monospace"));
		}

		if (m_render_mode == FT_RENDER_MODE_LCD)
		{
			FT_Library_SetLcdFilter(m_font_library, FT_LCD_FILTER_DEFAULT);
		}
	}

	TrueTypeTileset::~TrueTypeTileset()
	{
		if (m_font_face)
		{
			FT_Done_Face(m_font_face);
			m_font_face = nullptr;
		}

		if (m_font_library)
		{
			FT_Done_FreeType(m_font_library);
			m_font_library = nullptr;
		}
	}

	void TrueTypeTileset::Remove()
	{
		for (auto i: m_tiles) // TODO: move to base class
		{
			if (m_container.slots.count(i.first) && m_container.slots[i.first].get() == i.second.get())
			{
				m_container.slots.erase(i.first);
			}

			m_container.atlas.Remove(i.second);
		}
	}

	bool TrueTypeTileset::Save()
	{
		// Do nothing. TrueType tileset is a dynamic tileset and provides tiles on-demand.
		return true;
	}

	void TrueTypeTileset::Reload(TrueTypeTileset&& tileset)
	{
		throw std::runtime_error("TrueTypeTileset::Reload: NYI"); // FIXME: NYI
	}

	Size TrueTypeTileset::GetBoundingBoxSize()
	{
		return m_tile_size;
	}

	bool TrueTypeTileset::Provides(uint16_t code)
	{
		if (code < m_base_code) return false;
		uint16_t unicode = m_codepage->Convert((int)(code-m_base_code));
		if (unicode == kUnicodeReplacementCharacter) return false;
		return FT_Get_Char_Index(m_font_face, unicode) > 0;
	}

	void TrueTypeTileset::Prepare(uint16_t code)
	{
		PrepareBitmap(code);
	}

	Bitmap TrueTypeTileset::PrepareBitmap(uint16_t code)
	{
		if (m_tiles.count(code))
		{
			m_container.slots[code] = std::dynamic_pointer_cast<Slot>(m_tiles[code]);
		}

		if (code < m_base_code)
		{
			throw std::runtime_error("TrueTypeTileset: request for a tile that is not provided by the tileset");
		}

		int index = 0;
		uint16_t unicode = m_codepage->Convert((int)(code-m_base_code));
		if (unicode == kUnicodeReplacementCharacter || (index = FT_Get_Char_Index(m_font_face, unicode)) == 0)
		{
			throw std::runtime_error("TrueTypeTileset: request for a tile that is not provided by the tileset");
		}

		if (FT_Load_Glyph(m_font_face, index, FT_LOAD_FORCE_AUTOHINT))
		{
			throw std::runtime_error("TrueTypeTileset: can't load character glyph");
		}

		if (m_font_face->glyph->format != FT_GLYPH_FORMAT_BITMAP)
		{
			FT_Render_Mode render_mode = m_render_mode;

			if (FT_Render_Glyph(m_font_face->glyph, render_mode) != 0)
			{
				throw std::runtime_error("TrueTypeTileset: can't render glyph");
			}
		}

		FT_GlyphSlot& slot = m_font_face->glyph;

		int rows = slot->bitmap.rows;
		int columns = 0;
		int pixel_size = 0;

		int height = m_font_face->size->metrics.height >> 6;
		int descender = m_font_face->size->metrics.descender >> 6;
		int bx = (slot->metrics.horiBearingX >> 6) / 64;
		int by = slot->metrics.horiBearingY >> 6;

		if (slot->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY)
		{
			columns = slot->bitmap.width;
			pixel_size = 1;
		}
		else if (slot->bitmap.pixel_mode == FT_PIXEL_MODE_LCD)
		{
			columns = slot->bitmap.width/3;
			pixel_size = 3;
		}
		else if (slot->bitmap.pixel_mode == FT_PIXEL_MODE_MONO)
		{
			columns = slot->bitmap.width;
			pixel_size = 0;
		}

		Bitmap glyph(Size(columns, rows), Color(0, 0, 0, 0));

		for (int y = 0; y < rows; y++)
		{
			for (int x = 0; x < columns; x++)
			{
				uint8_t* p = slot->bitmap.buffer + y*slot->bitmap.pitch + x*pixel_size;
				if (slot->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY)
				{
					Color c(p[0], 255, 255, 255);
					glyph(x, y) = c;
				}
				else if (slot->bitmap.pixel_mode == FT_PIXEL_MODE_LCD)
				{
					Color c(255, p[2], p[1], p[0]);
					glyph(x, y) = c;
				}
				else if (slot->bitmap.pixel_mode == FT_PIXEL_MODE_MONO)
				{
					int j = x%8;
					int i = (x-j)/8;
					uint8_t byte = *(slot->bitmap.buffer + y*slot->bitmap.pitch + i);
					uint8_t alpha = (byte & (1 << (7-j)))? 255: 0;
					glyph(x, y) = Color(alpha, 255, 255, 255);
				}
			}
		}

		int descender2 = m_font_face->size->metrics.descender >> 6;
		int dy = -((by-descender2) - m_tile_size.height/2);
		Point offset;
		if (m_monospace)
		{
			offset = Point(-m_tile_size.width/2+bx, dy);
		}
		else
		{
			offset = Point(-(columns+bx)/2, dy);
		}

		LOG(Trace, L"Rasterized glyph for code " << (int)code << L" (unicode " << (int)unicode << L"), size = " << glyph.GetSize() << ", bx=" << bx << ", by=" << by << L", offset = " << offset);

		auto tile_slot = m_container.atlas.Add(glyph, Rectangle(glyph.GetSize()));
		tile_slot->offset = offset;
		tile_slot->alignment = Tile::Alignment::Center;
		tile_slot->bounds = m_bbox_size;
		m_tiles[code] = tile_slot;
		m_container.slots[code] = tile_slot;

		return glyph;
	}
}