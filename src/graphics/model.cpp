/************************************************************************/
/*                                                                      */
/* This file is part of VDrift.                                         */
/*                                                                      */
/* VDrift is free software: you can redistribute it and/or modify       */
/* it under the terms of the GNU General Public License as published by */
/* the Free Software Foundation, either version 3 of the License, or    */
/* (at your option) any later version.                                  */
/*                                                                      */
/* VDrift is distributed in the hope that it will be useful,            */
/* but WITHOUT ANY WARRANTY; without even the implied warranty of       */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        */
/* GNU General Public License for more details.                         */
/*                                                                      */
/* You should have received a copy of the GNU General Public License    */
/* along with VDrift.  If not, see <http://www.gnu.org/licenses/>.      */
/*                                                                      */
/************************************************************************/

#include "model.h"
#include "utils.h"
#include "vertexattribs.h"
#include "glutil.h"
#include <limits>

using namespace VERTEX_ATTRIBS;

#define ERROR_CHECK GLUTIL::CheckForOpenGLErrors(std::string(__PRETTY_FUNCTION__)+":"+__FILE__+":"+UTILS::tostr(__LINE__), error_output)

static const std::string file_magic = "OGLVARRAYV01";

static const bool vaoDebug = false;

MODEL::MODEL() :
	vao(0),
	elementVbo(0),
	elementCount(0),
	listid(0),
	radius(0),
	generatedmetrics(false),
	generatedvao(false)
{
	// Constructor.
}

MODEL::MODEL(const std::string & filepath, std::ostream & error_output) :
	vao(0),
	elementVbo(0),
	elementCount(0),
	listid(0),
	radius(0),
	generatedmetrics(false),
	generatedvao(false)
{
	if (filepath.size() > 4 && filepath.substr(filepath.size()-4) == ".ova")
		ReadFromFile(filepath, error_output, false);
	else
		Load(filepath, error_output, false);
}

MODEL::~MODEL()
{
	Clear();
}

bool MODEL::CanSave() const
{
	return false;
}

bool MODEL::Save(const std::string & strFileName, std::ostream & error_output) const
{
	return false;
}

bool MODEL::Load(const std::string & strFileName, std::ostream & error_output, bool genlist)
{
	return false;
}

bool MODEL::Load(const VERTEXARRAY & varray, std::ostream & error_output, bool genlist)
{
	BuildFromVertexArray(varray);
	if (genlist)
		GenerateListID(error_output);
	else
		GenerateVertexArrayObject(error_output);
	return true;
}

bool MODEL::Serialize(joeserialize::Serializer & s)
{
	_SERIALIZE_(s, m_mesh);
	return true;
}

bool MODEL::WriteToFile(const std::string & filepath)
{
	std::ofstream fileout(filepath.c_str());
	if (!fileout)
		return false;

	fileout.write(file_magic.c_str(), file_magic.size());
	joeserialize::BinaryOutputSerializer s(fileout);
	return Serialize(s);
}

bool MODEL::ReadFromFile(const std::string & filepath, std::ostream & error_output, bool generatelistid)
{
	std::ifstream filein(filepath.c_str(), std::ios_base::binary);
	if (!filein)
	{
		error_output << "Can't find file: " << filepath << std::endl;
		return false;
	}

	std::vector<char> fmagic(file_magic.size() + 1, 0);
	filein.read(&fmagic[0], file_magic.size());
	if (!filein)
	{
		error_output << "File magic read error: " << filepath << std::endl;
		return false;
	}

	if (!file_magic.compare(&fmagic[0]))
	{
		error_output << "File magic is incorrect: \"" << file_magic << "\" != \"" << &fmagic[0] << "\" in " << filepath << std::endl;
		return false;
	}

	joeserialize::BinaryInputSerializer s(filein);
	if (!Serialize(s))
	{
		error_output << "Serialization error: " << filepath << std::endl;
		Clear();
		return false;
	}

	ClearListID();
	ClearMetrics();
	GenerateMeshMetrics();

	if (generatelistid)
		GenerateListID(error_output);

	return true;
}

void MODEL::GenerateListID(std::ostream & error_output)
{
	if (HaveListID())
		return;

	ClearListID();
	listid = glGenLists(1);
	GLUTIL::CheckForOpenGLErrors("MODEL::GenerateListID gen list", error_output);

	const int * faces;
	const float * verts;
	const float * norms;
	const float * tcoord;
	int facecount;
	int vertcount;
	int normcount;
	int tccount;

	m_mesh.GetFaces(faces, facecount);
	m_mesh.GetVertices(verts, vertcount);
	m_mesh.GetNormals(norms, normcount);
	m_mesh.GetTexCoords(0, tcoord, tccount);

	assert(facecount > 0);
	assert(vertcount > 0);
	assert(normcount > 0);
	assert(tccount > 0);
	
	// mesa vertex attribute segfault fix
	GLint n = 0;
	glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &n);
	for (GLint i = 0; i < n; ++i)
		glDisableVertexAttribArray(i);

	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_NORMAL_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);

	glVertexPointer(3, GL_FLOAT, 0, verts);
	glNormalPointer(GL_FLOAT, 0, norms);
	glTexCoordPointer(2, GL_FLOAT, 0, tcoord);
	
	glNewList(listid, GL_COMPILE);
	glDrawElements(GL_TRIANGLES, facecount, GL_UNSIGNED_INT, faces);
	glEndList();

	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_NORMAL_ARRAY);
	glDisableClientState(GL_VERTEX_ARRAY);

	GLUTIL::CheckForOpenGLErrors("MODEL::GenerateListID init list", error_output);
}

template <typename T>
GLuint GenerateBufferObject(
	std::ostream & error_output,
	unsigned attribId,
	const T * data,
	unsigned vertexCount,
	unsigned elementsPerVertex,
	GLenum type = GL_FLOAT,
	bool normalized = false)
{
	GLuint vboHandle;
	glGenBuffers(1, &vboHandle);ERROR_CHECK;
	glBindBuffer(GL_ARRAY_BUFFER, vboHandle);ERROR_CHECK;
	glBufferData(GL_ARRAY_BUFFER, vertexCount*elementsPerVertex*sizeof(T), data, GL_STATIC_DRAW);ERROR_CHECK;
	glVertexAttribPointer(attribId, elementsPerVertex, type, normalized, 0, 0);ERROR_CHECK;
	glEnableVertexAttribArray(attribId);ERROR_CHECK;

	return vboHandle;
}

void MODEL::GenerateVertexArrayObject(std::ostream & error_output)
{
	if (generatedvao)
		return;

	// Generate vertex array object.
	glGenVertexArrays(1, &vao);ERROR_CHECK;
    if (vaoDebug)
        std::cout << "created vao " << vao << std::endl;
	glBindVertexArray(vao);ERROR_CHECK;

	// Buffer object for faces.
	const int * faces;
	int facecount;
	m_mesh.GetFaces(faces, facecount);
	assert(faces && facecount > 0);
	glGenBuffers(1, &elementVbo);ERROR_CHECK;
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, elementVbo);ERROR_CHECK;
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, facecount*sizeof(GLuint), faces, GL_STATIC_DRAW);ERROR_CHECK;
	elementCount = facecount;

	// Calculate the number of vertices (vertcount is the size of the verts array).
	const float * verts;
	int vertcount;
	m_mesh.GetVertices(verts, vertcount);
	assert(verts && vertcount > 0);
	unsigned int vertexCount = vertcount/3;

	// Generate buffer object for vertex positions.
	vbos.push_back(GenerateBufferObject(error_output, VERTEX_POSITION, verts, vertexCount, 3));

	// Generate buffer object for normals.
	const float * norms;
	int normcount;
	m_mesh.GetNormals(norms, normcount);
	if (!norms || normcount <= 0)
		glDisableVertexAttribArray(VERTEX_NORMAL);
	else
	{
		assert((unsigned int)normcount == vertexCount*3);
		vbos.push_back(GenerateBufferObject(error_output, VERTEX_NORMAL, norms, vertexCount, 3));
	}

	// TODO: Generate tangent and bitangent.
	glDisableVertexAttribArray(VERTEX_TANGENT);
	glDisableVertexAttribArray(VERTEX_BITANGENT);

	// Generate buffer object for colors.
	const unsigned char * cols = 0;
	int colcount = 0;
	m_mesh.GetColors(cols, colcount);
	if (cols && colcount)
	{
		assert((unsigned int)colcount == vertexCount*4);
		vbos.push_back(GenerateBufferObject(error_output, VERTEX_COLOR, cols, vertexCount, 4, GL_UNSIGNED_BYTE, true));
	}
	else
		glDisableVertexAttribArray(VERTEX_COLOR);

	// Generate buffer object for texture coordinates.
	const float * tc[1];
	int tccount[1];
	if (m_mesh.GetTexCoordSets() > 0)
	{
		// TODO: Make this work for UV1 and UV2.
		m_mesh.GetTexCoords(0, tc[0], tccount[0]);
		assert((unsigned int)tccount[0] == vertexCount*2);
		vbos.push_back(GenerateBufferObject(error_output, VERTEX_UV0, tc[0], vertexCount, 2));
	}
	else
		glDisableVertexAttribArray(VERTEX_UV0);

	glDisableVertexAttribArray(VERTEX_UV1);
	glDisableVertexAttribArray(VERTEX_UV2);

	// Don't leave anything bound.
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER,0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,0);

	generatedvao = true;
}

bool MODEL::HaveVertexArrayObject() const
{
	return generatedvao;
}

void MODEL::ClearVertexArrayObject()
{
	if (generatedvao)
	{
		glBindBuffer(GL_ARRAY_BUFFER,0);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,0);
		if (!vbos.empty())
			glDeleteBuffers(vbos.size(), &vbos[0]);

		if (elementVbo != 0)
		{
			glDeleteBuffers(1, &elementVbo);
			elementVbo = 0;
		}
		if (vao != 0)
		{
			glBindVertexArray(0);
            if (vaoDebug)
                std::cout << "deleting vao " << vao << std::endl;
			glDeleteVertexArrays(1,&vao);
			vao = 0;
		}
	}
	listid = 0;
}

bool MODEL::GetVertexArrayObject(GLuint & vao_out, unsigned int & elementCount_out) const
{
	if (!generatedvao)
		return false;

	vao_out = vao;
	elementCount_out = elementCount;

	return true;
}

void MODEL::GenerateMeshMetrics()
{
	const float flt_max = std::numeric_limits<float>::max();
	const float flt_min = std::numeric_limits<float>::min();
	float maxv[3] = {flt_min, flt_min, flt_min};
	float minv[3] = {flt_max, flt_max, flt_max};

	const float * verts;
	int vnum3;
	m_mesh.GetVertices(verts, vnum3);
	assert(vnum3);

	for (int n = 0; n < vnum3; n += 3)
	{
		const float * v = verts + n;
		if (v[0] > maxv[0]) maxv[0] = v[0];
		if (v[1] > maxv[1]) maxv[1] = v[1];
		if (v[2] > maxv[2]) maxv[2] = v[2];
		if (v[0] < minv[0]) minv[0] = v[0];
		if (v[1] < minv[1]) minv[1] = v[1];
		if (v[2] < minv[2]) minv[2] = v[2];
	}

	min.Set(minv[0], minv[1], minv[2]);
	max.Set(maxv[0], maxv[1], maxv[2]);
	radius = GetSize().Magnitude() * 0.5f + 0.001f;	// 0.001 margin

	generatedmetrics = true;
}

void MODEL::ClearMeshData()
{
	m_mesh.Clear();
}

unsigned MODEL::GetListID() const
{
	RequireListID();
	return listid;
}

MATHVECTOR <float, 3> MODEL::GetSize() const
{
	return max - min;
}

MATHVECTOR <float, 3> MODEL::GetCenter() const
{
	return (max + min) * 0.5f;
}

float MODEL::GetRadius() const
{
	RequireMetrics();
	return radius;
}

bool MODEL::HaveMeshData() const
{
	return (m_mesh.GetNumFaces() > 0);
}

bool MODEL::HaveMeshMetrics() const
{
	return generatedmetrics;
}

bool MODEL::HaveListID() const
{
	return listid;
}

void MODEL::Clear()
{
	ClearMeshData();
	ClearListID();
	ClearVertexArrayObject();
	ClearMetrics();
}

const VERTEXARRAY & MODEL::GetVertexArray() const
{
	return m_mesh;
}

void MODEL::SetVertexArray(const VERTEXARRAY & newmesh)
{
	Clear();
	m_mesh = newmesh;
}

void MODEL::BuildFromVertexArray(const VERTEXARRAY & newmesh)
{
	SetVertexArray(newmesh);
	GenerateMeshMetrics();
}

bool MODEL::Loaded()
{
	return (m_mesh.GetNumFaces() > 0);
}

void MODEL::RequireMetrics() const
{
	// Mesh metrics need to be generated before they can be queried.
	assert(generatedmetrics);
}

void MODEL::RequireListID() const
{
	// Mesh id needs to be generated.
	assert(listid);
}

void MODEL::ClearListID()
{
	if (listid)
		glDeleteLists(listid, 1);
	listid = 0;
}

void MODEL::ClearMetrics()
{
	generatedmetrics = false;
}
