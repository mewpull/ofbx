struct Header {
	uint8 magic[21];
	uint8 reserved[2];
	uint32 version;
};

// We might care starting here... but probs not

struct Cursor {
	const uint8* current;
	const uint8* begin;
	const uint8* end;
};

static void setTranslation(const Vec3& t, Matrix* mtx) {
	mtx.m[12] = t.x;
	mtx.m[13] = t.y;
	mtx.m[14] = t.z;
}

template <typename T> static bool parseBinaryArray(const Property& property, std::vector<T>* out);

static int resolveEnumProperty(const Object& object, const char* name, int default_value) {
	Element* element = (Element*)resolveProperty(object, name);
	if (!element) return default_value;
	Property* x = (Property*)element.getProperty(4);
	if (!x) return default_value;

	return x.value.toInt();
}

static Vec3 resolveVec3Property(const Object& object, const char* name, const Vec3& default_value) {
	Element* element = (Element*)resolveProperty(object, name);
	if (!element) return default_value;
	Property* x = (Property*)element.getProperty(4);
	if (!x || !x.next || !x.next.next) return default_value;

	return {x.value.toDouble(), x.next.value.toDouble(), x.next.next.value.toDouble()};
}

Object::Object(const Scene& _scene, const IElement& _element)
	: scene(_scene)
	, element(_element)
	, is_node(false)
	, node_attribute(nullptr) {
	auto& e = (Element&)_element;
	if (e.first_property && e.first_property.next) {
		e.first_property.next.value.toString(name);
	}
	else {
		name[0] = '\0';
	}
}

template <typename T> static OptionalError<T> read(Cursor* cursor) {
	if (cursor.current + sizeof(T) > cursor.end) return Error("Reading past the end");
	T value = *(const T*)cursor.current;
	cursor.current += sizeof(T);
	return value;
}

static OptionalError<DataView> readShortString(Cursor* cursor) {
	DataView value;
	OptionalError<uint8> length = read<uint8>(cursor);
	if (length.isError()) return Error();

	if (cursor.current + length.getValue() > cursor.end) return Error("Reading past the end");
	value.begin = cursor.current;
	cursor.current += length.getValue();

	value.end = cursor.current;

	return value;
}

static OptionalError<DataView> readLongString(Cursor* cursor) {
	DataView value;
	OptionalError<uint32> length = read<uint32>(cursor);
	if (length.isError()) return Error();

	if (cursor.current + length.getValue() > cursor.end) return Error("Reading past the end");
	value.begin = cursor.current;
	cursor.current += length.getValue();

	value.end = cursor.current;

	return value;
}

static OptionalError<Property*> readProperty(Cursor* cursor) {
	if (cursor.current == cursor.end) return Error("Reading past the end");

	std::unique_ptr<Property> prop = std::make_unique<Property>();
	prop.next = nullptr;
	prop.type = *cursor.current;
	++cursor.current;
	prop.value.begin = cursor.current;

	switch (prop.type) {
		case 'S': {
			OptionalError<DataView> val = readLongString(cursor);
			if (val.isError()) return Error();
			prop.value = val.getValue();
			break;
		}
		case 'Y': cursor.current += 2; break;
		case 'C': cursor.current += 1; break;
		case 'I': cursor.current += 4; break;
		case 'F': cursor.current += 4; break;
		case 'D': cursor.current += 8; break;
		case 'L': cursor.current += 8; break;
		case 'R': {
			OptionalError<uint32> len = read<uint32>(cursor);
			if (len.isError()) return Error();
			if (cursor.current + len.getValue() > cursor.end) return Error("Reading past the end");
			cursor.current += len.getValue();
			break;
		}
		case 'b':
		case 'f':
		case 'd':
		case 'l':
		case 'i': {
			OptionalError<uint32> length = read<uint32>(cursor);
			OptionalError<uint32> encoding = read<uint32>(cursor);
			OptionalError<uint32> comp_len = read<uint32>(cursor);
			if (length.isError() | encoding.isError() | comp_len.isError()) return Error();
			if (cursor.current + comp_len.getValue() > cursor.end) return Error("Reading past the end");
			cursor.current += comp_len.getValue();
			break;
		}
		default: return Error("Unknown property type");
	}
	prop.value.end = cursor.current;
	return prop.release();
}

static void deleteElement(Element* el) {
	if (!el) return;

	delete el.first_property;
	deleteElement(el.child);
	Element* iter = el;
	// do not use recursion to avoid stack overflow
	do {
		Element* next = iter.sibling;
		delete iter;
		iter = next;
	} while (iter);
}

static OptionalError<uint64> readElementOffset(Cursor* cursor, uint16 version) {
	if (version >= 7500) {
		OptionalError<uint64> tmp = read<uint64>(cursor);
		if (tmp.isError()) return Error();
		return tmp.getValue();
	}

	OptionalError<uint32> tmp = read<uint32>(cursor);
	if (tmp.isError()) return Error();
	return tmp.getValue();
}

static OptionalError<Element*> readElement(Cursor* cursor, uint32 version) {
	OptionalError<uint64> end_offset = readElementOffset(cursor, version);
	if (end_offset.isError()) return Error();
	if (end_offset.getValue() == 0) return nullptr;

	OptionalError<uint64> prop_count = readElementOffset(cursor, version);
	OptionalError<uint64> prop_length = readElementOffset(cursor, version);
	if (prop_count.isError() || prop_length.isError()) return Error();

	const char* sbeg = 0;
	const char* send = 0;
	OptionalError<DataView> id = readShortString(cursor);
	if (id.isError()) return Error();

	Element* element = new Element();
	element.first_property = nullptr;
	element.id = id.getValue();

	element.child = nullptr; 
	element.sibling = nullptr;

	Property** prop_link = &element.first_property;
	for (uint32 i = 0; i < prop_count.getValue(); ++i) {
		OptionalError<Property*> prop = readProperty(cursor);
		if (prop.isError()) {
			deleteElement(element);
			return Error();
		}

		*prop_link = prop.getValue();
		prop_link = &(*prop_link).next;
	}

	if (cursor.current - cursor.begin >= (ptrdiff_t)end_offset.getValue()) return element;

	int BLOCK_SENTINEL_LENGTH = version >= 7500 ? 25 : 13;

	Element** link = &element.child;
	while (cursor.current - cursor.begin < ((ptrdiff_t)end_offset.getValue() - BLOCK_SENTINEL_LENGTH)) {
		OptionalError<Element*> child = readElement(cursor, version);
		if (child.isError()) {
			deleteElement(element);
			return Error();
		}

		*link = child.getValue();
		link = &(*link).sibling;
	}

	if (cursor.current + BLOCK_SENTINEL_LENGTH > cursor.end) {
		deleteElement(element); 
		return Error("Reading past the end");
	}

	cursor.current += BLOCK_SENTINEL_LENGTH;
	return element;
}

static bool isEndLine(const Cursor& cursor) {
	return *cursor.current == '\n';
}

static void skipInsignificantWhitespaces(Cursor* cursor) {
	while (cursor.current < cursor.end && isspace(*cursor.current) && *cursor.current != '\n') {
		++cursor.current;
	}
}

static void skipLine(Cursor* cursor) {
	while (cursor.current < cursor.end && !isEndLine(*cursor)) {
		++cursor.current;
	}
	if (cursor.current < cursor.end) ++cursor.current;
	skipInsignificantWhitespaces(cursor);
}

static void skipWhitespaces(Cursor* cursor) {
	while (cursor.current < cursor.end && isspace(*cursor.current)) {
		++cursor.current;
	}
	while (cursor.current < cursor.end && *cursor.current == ';') skipLine(cursor);
}

static bool isTextTokenChar(char c) {
	return isalnum(c) || c == '_';
}

static DataView readTextToken(Cursor* cursor) {
	DataView ret;
	ret.begin = cursor.current;
	while (cursor.current < cursor.end && isTextTokenChar(*cursor.current)) {
		++cursor.current;
	}
	ret.end = cursor.current;
	return ret;
}

static OptionalError<Property*> readTextProperty(Cursor* cursor) {
	std::unique_ptr<Property> prop = std::make_unique<Property>();
	prop.value.is_binary = false;
	prop.next = nullptr;
	if (*cursor.current == '"') {
		prop.type = 'S';
		++cursor.current;
		prop.value.begin = cursor.current;
		while (cursor.current < cursor.end && *cursor.current != '"') {
			++cursor.current;
		}
		prop.value.end = cursor.current;
		if (cursor.current < cursor.end) ++cursor.current; // skip '"'
		return prop.release();
	}
	
	if (isdigit(*cursor.current) || *cursor.current == '-') {
		prop.type = 'L';
		prop.value.begin = cursor.current;
		if (*cursor.current == '-') ++cursor.current;
		while (cursor.current < cursor.end && isdigit(*cursor.current)) {
			++cursor.current;
		}
		prop.value.end = cursor.current;

		if (cursor.current < cursor.end && *cursor.current == '.') {
			prop.type = 'D';
			++cursor.current;
			while (cursor.current < cursor.end && isdigit(*cursor.current)) {
				++cursor.current;
			}
			if (cursor.current < cursor.end && (*cursor.current == 'e' || *cursor.current == 'E')) {
				// 10.5e-013
				++cursor.current;
				if (cursor.current < cursor.end && *cursor.current == '-') ++cursor.current;
				while (cursor.current < cursor.end && isdigit(*cursor.current)) ++cursor.current;
			}

			prop.value.end = cursor.current;
		}
		return prop.release();
	}
	
	if (*cursor.current == 'T' || *cursor.current == 'Y') {
		// WTF is this
		prop.type = *cursor.current;
		prop.value.begin = cursor.current;
		++cursor.current;
		prop.value.end = cursor.current;
		return prop.release();
	}

	if (*cursor.current == '*') {
		prop.type = 'l';
		++cursor.current;
		// Vertices: *10740 { a: 14.2760353088379,... }
		while (cursor.current < cursor.end && *cursor.current != ':') {
			++cursor.current;
		}
		if (cursor.current < cursor.end) ++cursor.current; // skip ':'
		skipInsignificantWhitespaces(cursor);
		prop.value.begin = cursor.current;
		prop.count = 0;
		bool is_any = false;
		while (cursor.current < cursor.end && *cursor.current != '}') {
			if (*cursor.current == ',') {
				if (is_any) ++prop.count;
				is_any = false;
			}
			else if (!isspace(*cursor.current) && *cursor.current != '\n') is_any = true;
			if (*cursor.current == '.') prop.type = 'd';
			++cursor.current;
		}
		if (is_any) ++prop.count;
		prop.value.end = cursor.current;
		if (cursor.current < cursor.end) ++cursor.current; // skip '}'
		return prop.release();
	}

	assert(false);
	return Error("TODO");
}

static OptionalError<Element*> readTextElement(Cursor* cursor) {
	DataView id = readTextToken(cursor);
	if (cursor.current == cursor.end) return Error("Unexpected end of file");
	if(*cursor.current != ':') return Error("Unexpected end of file");
	++cursor.current;

	skipWhitespaces(cursor);
	if (cursor.current == cursor.end) return Error("Unexpected end of file");

	Element* element = new Element;
	element.id = id;

	Property** prop_link = &element.first_property;
	while (cursor.current < cursor.end && *cursor.current != '\n' && *cursor.current != '{') {
		OptionalError<Property*> prop = readTextProperty(cursor);
		if (prop.isError()) {
			deleteElement(element);
			return Error();
		}
		if (cursor.current < cursor.end && *cursor.current == ',') {
			++cursor.current;
			skipWhitespaces(cursor);
		}
		skipInsignificantWhitespaces(cursor);

		*prop_link = prop.getValue();
		prop_link = &(*prop_link).next;
	}
	
	Element** link = &element.child;
	if (*cursor.current == '{') {
		++cursor.current;
		skipWhitespaces(cursor);
		while (cursor.current < cursor.end && *cursor.current != '}') {
			OptionalError<Element*> child = readTextElement(cursor);
			if (child.isError()) {
				deleteElement(element);
				return Error();
			}
			skipWhitespaces(cursor);

			*link = child.getValue();
			link = &(*link).sibling;
		}
		if (cursor.current < cursor.end) ++cursor.current; // skip '}'
	}
	return element;
}

static OptionalError<Element*> tokenizeText(const uint8* data, size_t size) {
	Cursor cursor;
	cursor.begin = data;
	cursor.current = data;
	cursor.end = data + size;

	Element* root = new Element();
	root.first_property = nullptr;
	root.id.begin = nullptr;
	root.id.end = nullptr;
	root.child = nullptr;
	root.sibling = nullptr;

	Element** element = &root.child;
	while (cursor.current < cursor.end) {
		if (*cursor.current == ';' || *cursor.current == '\r' || *cursor.current == '\n') {
			skipLine(&cursor);
		}
		else {
			OptionalError<Element*> child = readTextElement(&cursor);
			if (child.isError()) {
				deleteElement(root);
				return Error();
			}
			*element = child.getValue();
			if (!*element) return root;
			element = &(*element).sibling;
		}
	}

	return root;
}

static OptionalError<Element*> tokenize(const uint8* data, size_t size) {
	Cursor cursor;
	cursor.begin = data;
	cursor.current = data;
	cursor.end = data + size;

	const Header* header = (const Header*)cursor.current;
	cursor.current += sizeof(*header);

	Element* root = new Element();
	root.first_property = nullptr;
	root.id.begin = nullptr;
	root.id.end = nullptr;
	root.child = nullptr;
	root.sibling = nullptr;

	Element** element = &root.child;
	for (;;) {
		OptionalError<Element*> child = readElement(&cursor, header.version);
		if (child.isError()) {
			deleteElement(root);
			return Error();
		}
		*element = child.getValue();
		if (!*element) return root;
		element = &(*element).sibling;
	}
}

Material::Material(const Scene& _scene, const IElement& _element)
	: Object(_scene, _element) {
}

struct MaterialImpl : Material {
	MaterialImpl(const Scene& _scene, const IElement& _element)
		: Material(_scene, _element) {
		for (const Texture*& tex : textures) tex = nullptr;
	}

	Type getType() const override { return Type::MATERIAL; }

	const Texture* getTexture(Texture::TextureType type) const override { return textures[type]; }
	Color getDiffuseColor() const override { return diffuse_color; }

	const Texture* textures[Texture::TextureType::COUNT];
	Color diffuse_color;
};

struct LimbNodeImpl : Object {
	LimbNodeImpl(const Scene& _scene, const IElement& _element)
		: Object(_scene, _element) {
		is_node = true;
	}
	Type getType() const override { return Type::LIMB_NODE; }
};

struct NullImpl : Object {
	NullImpl(const Scene& _scene, const IElement& _element)
		: Object(_scene, _element) {
		is_node = true;
	}
	Type getType() const override { return Type::NULL_NODE; }
};

struct Root : Object {
	Root(const Scene& _scene, const IElement& _element)
		: Object(_scene, _element) {
		copyString(name, "RootNode");
		is_node = true;
	}
	Type getType() const override { return Type::ROOT; }
};

template <typename T> const char* fromString(const char* str, const char* end, T* val);
template <> const char* fromString<int>(const char* str, const char* end, int* val) {
	*val = atoi(str);
	const char* iter = str;
	while (iter < end && *iter != ',') ++iter;
	if (iter < end) ++iter; // skip ','
	return (const char*)iter;
}

template <> const char* fromString<uint64>(const char* str, const char* end, uint64* val) {
	*val = strtoull(str, nullptr, 10);
	const char* iter = str;
	while (iter < end && *iter != ',') ++iter;
	if (iter < end) ++iter; // skip ','
	return (const char*)iter;
}

template <> const char* fromString<int64>(const char* str, const char* end, int64* val) {
	*val = atoll(str);
	const char* iter = str;
	while (iter < end && *iter != ',') ++iter;
	if (iter < end) ++iter; // skip ','
	return (const char*)iter;
}

template <> const char* fromString<double>(const char* str, const char* end, double* val) {
	*val = atof(str);
	const char* iter = str;
	while (iter < end && *iter != ',') ++iter;
	if (iter < end) ++iter; // skip ','
	return (const char*)iter;
}

template <> const char* fromString<float>(const char* str, const char* end, float* val) {
	*val = (float)atof(str);
	const char* iter = str;
	while (iter < end && *iter != ',') ++iter;
	if (iter < end) ++iter; // skip ','
	return (const char*)iter;
}

const char* fromString(const char* str, const char* end, double* val, int count) {
	const char* iter = str;
	for (int i = 0; i < count; ++i) {
		*val = atof(iter);
		++val;
		while (iter < end && *iter != ',') ++iter;
		if (iter < end) ++iter; // skip ','

		if (iter == end) return iter;

	}
	return (const char*)iter;
}

template <> const char* fromString<Vec2>(const char* str, const char* end, Vec2* val) {
	return fromString(str, end, &val.x, 2);
}

template <> const char* fromString<Vec3>(const char* str, const char* end, Vec3* val) {
	return fromString(str, end, &val.x, 3);
}

template <> const char* fromString<Vec4>(const char* str, const char* end, Vec4* val) {
	return fromString(str, end, &val.x, 4);
}

template <> const char* fromString<Matrix>(const char* str, const char* end, Matrix* val) {
	return fromString(str, end, &val.m[0], 16);
}



template <typename T>
static void splat(std::vector<T>* out,
	GeometryImpl::VertexDataMapping mapping,
	const std::vector<T>& data,
	const std::vector<int>& indices,
	const std::vector<int>& original_indices) {
	assert(out);
	assert(!data.empty());

	if (mapping == GeometryImpl::BY_POLYGON_VERTEX) {
		if (indices.empty()) {
			out.resize(data.size());
			memcpy(&(*out)[0], &data[0], sizeof(data[0]) * data.size());
		}
		else {
			out.resize(indices.size());
			int data_size = (int)data.size();
			for (int i = 0, c = (int)indices.size(); i < c; ++i) {
				if(indices[i] < data_size) (*out)[i] = data[indices[i]];
				else (*out)[i] = T();
			}
		}
	}
	else if (mapping == GeometryImpl::BY_VERTEX) {
		//  v0  v1 ...
		// uv0 uv1 ...
		assert(indices.empty());

		out.resize(original_indices.size());

		int data_size = (int)data.size();
		for (int i = 0, c = (int)original_indices.size(); i < c; ++i) {
			int idx = original_indices[i];
			if (idx < 0) idx = -idx - 1;
			if(idx < data_size) (*out)[i] = data[idx];
			else (*out)[i] = T();
		}
	}
	else {
		assert(false);
	}
}

template <typename T> static void remap(std::vector<T>* out, const std::vector<int>& map) {
	if (out.empty()) return;

	std::vector<T> old;
	old.swap(*out);
	int old_size = (int)old.size();
	for (int i = 0, c = (int)map.size(); i < c; ++i) {
		if(map[i] < old_size) out.push_back(old[map[i]]);
		else out.push_back(T());
	}
}

static int getTriCountFromPoly(const std::vector<int>& indices, int* idx) {
	int count = 1;
	while (indices[*idx + 1 + count] >= 0) {
		++count;
	}

	*idx = *idx + 2 + count;
	return count;
}
