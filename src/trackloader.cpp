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

#include "trackloader.h"
#include "loadcollisionshape.h"
#include "physics/dynamicsworld.h"
#include "coordinatesystem.h"
#include "tobullet.h"
#include "k1999.h"
#include "content/contentmanager.h"
#include "graphics/textureinfo.h"

#define EXTBULLET

static inline std::istream & operator >> (std::istream & lhs, btVector3 & rhs)
{
	std::string str;
	for (int i = 0; i < 3 && !lhs.eof(); ++i)
	{
		std::getline(lhs, str, ',');
		std::stringstream s(str);
		s >> rhs[i];
	}
	return lhs;
}

static inline std::istream & operator >> (std::istream & lhs, std::vector<std::string> & rhs)
{
	std::string str;
	for (size_t i = 0; i < rhs.size() && !lhs.eof(); ++i)
	{
		std::getline(lhs, str, ',');
		std::stringstream s(str);
		s >> rhs[i];
	}
	return lhs;
}

static btIndexedMesh GetIndexedMesh(const MODEL & model)
{
	const float * vertices;
	int vcount;
	const int * faces;
	int fcount;
	model.GetVertexArray().GetVertices(vertices, vcount);
	model.GetVertexArray().GetFaces(faces, fcount);

	assert(fcount % 3 == 0); //Face count is not a multiple of 3

	btIndexedMesh mesh;
	mesh.m_numTriangles = fcount / 3;
	mesh.m_triangleIndexBase = (const unsigned char *)faces;
	mesh.m_triangleIndexStride = sizeof(int) * 3;
	mesh.m_numVertices = vcount;
	mesh.m_vertexBase = (const unsigned char *)vertices;
	mesh.m_vertexStride = sizeof(float) * 3;
	mesh.m_vertexType = PHY_FLOAT;
	return mesh;
}

TRACK::LOADER::LOADER(
	ContentManager & content,
	DynamicsWorld & world,
	DATA & data,
	std::ostream & info_output,
	std::ostream & error_output,
	const std::string & trackpath,
	const std::string & trackdir,
	const std::string & texturedir,
	const std::string & sharedobjectpath,
	const int anisotropy,
	const bool reverse,
	const bool dynamic_objects,
	const bool dynamic_shadows,
	const bool agressive_combining) :
	content(content),
	world(world),
	data(data),
	info_output(info_output),
	error_output(error_output),
	trackpath(trackpath),
	trackdir(trackdir),
	texturedir(texturedir),
	sharedobjectpath(sharedobjectpath),
	anisotropy(anisotropy),
	dynamic_objects(dynamic_objects),
	dynamic_shadows(dynamic_shadows),
	agressive_combining(agressive_combining),
	packload(false),
	numobjects(0),
	numloaded(0),
	params_per_object(17),
	expected_params(17),
	min_params(14),
	error(false),
	list(false),
	track_shape(0)
{
	objectpath = trackpath + "/objects";
	objectdir = trackdir + "/objects";
	data.reverse = reverse;
}

TRACK::LOADER::~LOADER()
{
	Clear();
}

void TRACK::LOADER::Clear()
{
	bodies.clear();
	combined.clear();
	objectfile.close();
	pack.Close();
}

bool TRACK::LOADER::BeginLoad()
{
	Clear();

	info_output << "Loading track from path: " << trackpath << std::endl;

	if (!LoadSurfaces())
	{
		info_output << "No Surfaces File. Continuing with standard surfaces" << std::endl;
	}

	if (!LoadRoads())
	{
		error_output << "Error during road loading; continuing with an unsmoothed track" << std::endl;
		data.roads.clear();
	}

	if (!CreateRacingLines())
	{
		return false;
	}

	// load info
	std::string info_path = trackpath + "/track.txt";
	std::ifstream file(info_path.c_str());
	if (!file.good())
	{
		error_output << "Can't find track configfile: " << info_path << std::endl;
		return false;
	}

	// parse info
	PTree info;
	read_ini(file, info);

	info.get("vertical tracking skyboxes", data.vertical_tracking_skyboxes);
	info.get("cull faces", data.cull);

	if (!LoadStartPositions(info))
	{
		return false;
	}

	if (!LoadLapSections(info))
	{
		return false;
	}

	if (!BeginObjectLoad())
	{
		return false;
	}

	return true;
}

bool TRACK::LOADER::ContinueLoad()
{
	if (data.loaded)
	{
		return true;
	}

	std::pair <bool, bool> loadstatus = ContinueObjectLoad();
	if (loadstatus.first)
	{
		return false;
	}

	if (!loadstatus.second)
	{
		if (agressive_combining)
		{
			std::map<std::string, OBJECT>::iterator i;
			for (i = combined.begin(); i != combined.end(); ++i)
			{
				std::tr1::shared_ptr<MODEL> & model = i->second.model;
				if (!model->HaveMeshMetrics())
				{
					// cache combined model
					content.load(model, objectdir, i->first, model->GetVertexArray());
				}
				AddObject(i->second);
			}
		}
#ifndef EXTBULLET
		btCollisionObject * track_object = new btCollisionObject();
		//track_shape->createAabbTreeFromChildren();
		track_object->setCollisionShape(track_shape);
		world.addCollisionObject(track_object);
		data.objects.push_back(track_object);
		data.shapes.push_back(track_shape);
		track_shape = 0;
#endif
		data.loaded = true;
		Clear();
	}

	return true;
}

bool TRACK::LOADER::BeginObjectLoad()
{
#ifndef EXTBULLET
	assert(track_shape == 0);
	track_shape = new btCompoundShape(true);
#endif

	list = true;
	packload = pack.Load(objectpath + "/objects.jpk");

	if (Begin())
	{
		list = false;
		return true;
	}

	std::string objectlist = objectpath + "/list.txt";
	objectfile.open(objectlist.c_str());
	if (!objectfile.good())
	{
		return false;
	}
	return BeginOld();
}

std::pair<bool, bool> TRACK::LOADER::ContinueObjectLoad()
{
	if (error)
	{
		return std::make_pair(true, false);
	}

	if (list)
	{
		return ContinueOld();
	}

	return Continue();
}

bool TRACK::LOADER::Begin()
{
	content.load(track_config, objectdir, "objects.txt");
	if (track_config.get())
	{
		nodes = 0;
		if (track_config->get("object", nodes))
		{
			node_it = nodes->begin();
			numobjects = nodes->size();
			data.models.reserve(numobjects);
			data.meshes.reserve(numobjects);
			return true;
		}
	}
	return false;
}

std::pair<bool, bool> TRACK::LOADER::Continue()
{
	if (node_it == nodes->end())
	{
		return std::make_pair(false, false);
	}

	if (!LoadNode(node_it->second))
	{
		return std::make_pair(true, false);
	}

	node_it++;

	return std::make_pair(false, true);
}

bool TRACK::LOADER::LoadModel(const std::string & name)
{
	std::tr1::shared_ptr<MODEL> model;
	if ((packload && content.load(model, objectdir, name, pack)) ||
		content.load(model, objectdir, name))
	{
		data.models.push_back(model);
		return true;
	}
	return false;
}

bool TRACK::LOADER::LoadShape(const PTree & cfg, const MODEL & model, BODY & body)
{
	if (body.mass < 1E-3)
	{
		btTriangleIndexVertexArray * mesh = new btTriangleIndexVertexArray();
		mesh->addIndexedMesh(GetIndexedMesh(model));
		data.meshes.push_back(mesh);
		body.mesh = mesh;

		int surface = 0;
		cfg.get("surface", surface);
		if (surface >= (int)data.surfaces.size())
		{
			surface = 0;
		}

		btBvhTriangleMeshShape * shape = new btBvhTriangleMeshShape(mesh, true);
		shape->setUserPointer((void*)&data.surfaces[surface]);
		data.shapes.push_back(shape);
		body.shape = shape;
	}
	else
	{
		btVector3 center(0, 0, 0);
		cfg.get("mass-center", center);
		btTransform transform = btTransform::getIdentity();
		transform.getOrigin() -= center;

		btCompoundShape * compound = 0;
		btCollisionShape * shape = 0;
		LoadCollisionShape(cfg, transform, shape, compound);

		if (!shape)
		{
			// fall back to model bounding box
			btVector3 size = ToBulletVector(model.GetSize());
			shape = new btBoxShape(size * 0.5);
			center = center + ToBulletVector(model.GetCenter());
		}
		if (compound)
		{
			shape = compound;
		}
		data.shapes.push_back(shape);

		shape->calculateLocalInertia(body.mass, body.inertia);
		body.shape = shape;
		body.center = center;
	}

	return true;
}

TRACK::LOADER::body_iterator TRACK::LOADER::LoadBody(const PTree & cfg)
{
	BODY body;
	std::string texture_name;
	std::string model_name;
	int clampuv = 0;
	bool mipmap = true;
	bool skybox = false;
	bool alphablend = false;
	bool doublesided = false;
	bool isashadow = false;

	cfg.get("texture", texture_name, error_output);
	cfg.get("model", model_name, error_output);
	cfg.get("clampuv", clampuv);
	cfg.get("mipmap", mipmap);
	cfg.get("skybox", skybox);
	cfg.get("alphablend", alphablend);
	cfg.get("doublesided", doublesided);
	cfg.get("isashadow", isashadow);
	cfg.get("nolighting", body.nolighting);

	std::vector<std::string> texture_names(3);
	std::stringstream s(texture_name);
	s >> texture_names;

	// set relative path for models and textures, ugly hack
	// need to identify body references
	std::string name;
	if (cfg.value() == "body" && cfg.parent())
	{
		name = cfg.parent()->value();
	}
	else
	{
		name = cfg.value();
		size_t npos = name.rfind("/");
		if (npos < name.length())
		{
			std::string rel_path = name.substr(0, npos+1);
			model_name = rel_path + model_name;
			texture_names[0] = rel_path + texture_names[0];
			if (!texture_names[1].empty()) texture_names[1] = rel_path + texture_names[1];
			if (!texture_names[2].empty()) texture_names[2] = rel_path + texture_names[2];
		}
	}

	if (dynamic_shadows && isashadow)
	{
		return bodies.end();
	}

	if (!LoadModel(model_name))
	{
		info_output << "Failed to load body " << cfg.value() << " model " << model_name << std::endl;
		return bodies.end();
	}

	MODEL & model = *data.models.back();

	body.collidable = cfg.get("mass", body.mass);
	if (body.collidable)
	{
		LoadShape(cfg, model, body);
	}

	// load textures
	TEXTUREINFO texinfo;
	texinfo.mipmap = mipmap || anisotropy; //always mipmap if anisotropy is on
	texinfo.anisotropy = anisotropy;
	texinfo.repeatu = clampuv != 1 && clampuv != 2;
	texinfo.repeatv = clampuv != 1 && clampuv != 3;

	std::tr1::shared_ptr<TEXTURE> diffuse, miscmap1, miscmap2;
	content.load(diffuse, objectdir, texture_names[0], texinfo);
	if (texture_names[1].length() > 0)
	{
		content.load(miscmap1, objectdir, texture_names[1], texinfo);
	}
	if (texture_names[2].length() > 0)
	{
		texinfo.normalmap = true;
		content.load(miscmap2, objectdir, texture_names[2], texinfo);
	}

	// setup drawable
	DRAWABLE & drawable = body.drawable;
	drawable.SetModel(model);
	drawable.SetDiffuseMap(diffuse);
	drawable.SetMiscMap1(miscmap1);
	drawable.SetMiscMap2(miscmap2);
	drawable.SetDecal(alphablend);
	drawable.SetCull(data.cull && !doublesided, false);
	drawable.SetRadius(model.GetRadius());
	drawable.SetObjectCenter(model.GetCenter());
	drawable.SetSkybox(skybox);
	drawable.SetVerticalTrack(skybox && data.vertical_tracking_skyboxes);

	return bodies.insert(std::make_pair(name, body)).first;
}

void TRACK::LOADER::AddBody(SCENENODE & scene, const BODY & body)
{
	bool nolighting = body.nolighting;
	bool alphablend = body.drawable.GetDecal();
	bool skybox = body.drawable.GetSkybox();
	keyed_container<DRAWABLE> * dlist = &scene.GetDrawlist().normal_noblend;
	if (alphablend)
	{
		dlist = &scene.GetDrawlist().normal_blend;
	}
	else if (nolighting)
	{
		dlist = &scene.GetDrawlist().normal_noblend_nolighting;
	}
	if (skybox)
	{
		if (alphablend)
		{
			dlist = &scene.GetDrawlist().skybox_blend;
		}
		else
		{
			dlist = &scene.GetDrawlist().skybox_noblend;
		}
	}
	dlist->insert(body.drawable);
}

bool TRACK::LOADER::LoadNode(const PTree & sec)
{
	const PTree * sec_body;
	if (!sec.get("body", sec_body, error_output))
	{
		return false;
	}

	body_iterator ib = LoadBody(*sec_body);
	if (ib == bodies.end())
	{
		//info_output << "Object " << sec.value() << " failed to load body" << std::endl;
		return true;
	}

	MATHVECTOR<float, 3> position, angle;
	bool has_transform = sec.get("position",  position) | sec.get("rotation", angle);
	QUATERNION<float> rotation(angle[0]/180*M_PI, angle[1]/180*M_PI, angle[2]/180*M_PI);

	const BODY & body = ib->second;
	if (body.mass < 1E-3)
	{
		// static geometry
		if (has_transform)
		{
			// static geometry instanced
			keyed_container <SCENENODE>::handle sh = data.static_node.AddNode();
			SCENENODE & node = data.static_node.GetNode(sh);
			node.GetTransform().SetTranslation(position);
			node.GetTransform().SetRotation(rotation);
			AddBody(node, body);
		}
		else
		{
			// static geometry pretransformed(non instanced)
			AddBody(data.static_node, body);
		}

		if (body.collidable)
		{
			// static geometry collidable
			btTransform transform;
			transform.setOrigin(ToBulletVector(position));
			transform.setRotation(ToBulletQuaternion(rotation));
#ifndef EXTBULLET
			track_shape->addChildShape(transform, body.shape);
#else
			btCollisionObject * object = new btCollisionObject();
			object->setActivationState(DISABLE_SIMULATION);
			object->setWorldTransform(transform);
			object->setCollisionShape(body.shape);
			object->setUserPointer(body.shape->getUserPointer());
			data.objects.push_back(object);
			world.addCollisionObject(object);
#endif
		}
	}
	else
	{
		// fix postion due to rotation around mass center
		MATHVECTOR<float, 3> center_local = ToMathVector<float>(body.center);
		MATHVECTOR<float, 3> center_world = center_local;
		rotation.RotateVector(center_world);
		position = position - center_local + center_world;

		if (dynamic_objects)
		{
			// dynamic geometry
			data.body_transforms.push_back(MotionState());
			data.body_transforms.back().rotation = ToBulletQuaternion(rotation);
			data.body_transforms.back().position = ToBulletVector(position);
			data.body_transforms.back().massCenterOffset = -body.center;

			btRigidBody::btRigidBodyConstructionInfo info(body.mass, &data.body_transforms.back(), body.shape, body.inertia);
			info.m_friction = 0.9;

			btRigidBody * object = new btRigidBody(info);
			object->setContactProcessingThreshold(0.0);
			data.objects.push_back(object);
			world.addRigidBody(object);

			keyed_container<SCENENODE>::handle node_handle = data.dynamic_node.AddNode();
			SCENENODE & node = data.dynamic_node.GetNode(node_handle);
			node.GetTransform().SetTranslation(position);
			node.GetTransform().SetRotation(rotation);
			data.body_nodes.push_back(node_handle);
			AddBody(node, body);
		}
		else
		{
			// dynamic geometry as static geometry collidable
			btTransform transform;
			transform.setOrigin(ToBulletVector(position));
			transform.setRotation(ToBulletQuaternion(rotation));

			btCollisionObject * object = new btCollisionObject();
			object->setActivationState(DISABLE_SIMULATION);
			object->setWorldTransform(transform);
			object->setCollisionShape(body.shape);
			object->setUserPointer(body.shape->getUserPointer());
			data.objects.push_back(object);
			world.addCollisionObject(object);

			keyed_container <SCENENODE>::handle sh = data.static_node.AddNode();
			SCENENODE & node = data.static_node.GetNode(sh);
			node.GetTransform().SetTranslation(position);
			node.GetTransform().SetRotation(rotation);
			AddBody(node, body);
		}
	}

	return true;
}

/// read from the file stream and put it in "output".
/// return true if the get was successful, else false
template <typename T>
static bool get(std::ifstream & f, T & output)
{
	if (!f.good()) return false;

	std::string instr;
	f >> instr;
	if (instr.empty()) return false;

	while (!instr.empty() && instr[0] == '#' && f.good())
	{
		f.ignore(1024, '\n');
		f >> instr;
	}

	if (!f.good() && !instr.empty() && instr[0] == '#') return false;

	std::stringstream sstr(instr);
	sstr >> output;
	return true;
}

void TRACK::LOADER::CalculateNumOld()
{
	numobjects = 0;
	std::string objectlist = objectpath + "/list.txt";
	std::ifstream f(objectlist.c_str());
	int params_per_object;
	if (get(f, params_per_object))
	{
		std::string junk;
		while (get(f, junk))
		{
			for (int i = 0; i < params_per_object-1; ++i)
			{
				get(f, junk);
			}
			numobjects++;
		}
	}
}

bool TRACK::LOADER::BeginOld()
{
	CalculateNumOld();

	data.models.reserve(numobjects);

	if (!get(objectfile, params_per_object))
	{
			return false;
	}

	if (params_per_object != expected_params)
	{
		error_output << "Track object list has " << params_per_object << " params per object, expected " << expected_params << std::endl;
		return false;
	}

	return true;
}

bool TRACK::LOADER::AddObject(const OBJECT & object)
{
	data.models.push_back(object.model);

	TEXTUREINFO texinfo;
	texinfo.mipmap = object.mipmap || anisotropy; //always mipmap if anisotropy is on
	texinfo.anisotropy = anisotropy;
	texinfo.repeatu = object.clamptexture != 1 && object.clamptexture != 2;
	texinfo.repeatv = object.clamptexture != 1 && object.clamptexture != 3;

	std::tr1::shared_ptr<TEXTURE> diffuse_texture;
	content.load(diffuse_texture, objectdir, object.texture, texinfo);

	std::tr1::shared_ptr<TEXTURE> miscmap1_texture;
	{
		std::string texture_name = object.texture.substr(0, std::max(0, (int)object.texture.length()-4)) + "-misc1.png";
		std::string filepath = objectpath + "/" + texture_name;
		if (std::ifstream(filepath.c_str()))
		{
			content.load(miscmap1_texture, objectdir, texture_name, texinfo);
		}
	}

	std::tr1::shared_ptr<TEXTURE> miscmap2_texture;
	{
		texinfo.normalmap = true;
		std::string texture_name = object.texture.substr(0, std::max(0, (int)object.texture.length()-4)) + "-misc2.png";
		std::string filepath = objectpath + "/" + texture_name;
		if (std::ifstream(filepath.c_str()))
		{
			content.load(miscmap2_texture, objectdir, texture_name, texinfo);
		}
	}

	//use a different drawlist layer where necessary
	bool transparent = (object.transparent_blend==1);
	keyed_container <DRAWABLE> * dlist = &data.static_node.GetDrawlist().normal_noblend;
	if (transparent)
	{
		dlist = &data.static_node.GetDrawlist().normal_blend;
	}
	else if (object.nolighting)
	{
		dlist = &data.static_node.GetDrawlist().normal_noblend_nolighting;
	}
	if (object.skybox)
	{
		if (transparent)
		{
			dlist = &data.static_node.GetDrawlist().skybox_blend;
		}
		else
		{
			dlist = &data.static_node.GetDrawlist().skybox_noblend;
		}
	}
	keyed_container <DRAWABLE>::handle dref = dlist->insert(DRAWABLE());
	DRAWABLE & drawable = dlist->get(dref);
	drawable.SetModel(*object.model);
	drawable.SetDiffuseMap(diffuse_texture);
	drawable.SetMiscMap1(miscmap1_texture);
	drawable.SetMiscMap2(miscmap2_texture);
	drawable.SetDecal(transparent);
	drawable.SetCull(data.cull && (object.transparent_blend!=2), false);
	drawable.SetRadius(object.model->GetRadius());
	drawable.SetObjectCenter(object.model->GetCenter());
	drawable.SetSkybox(object.skybox);
	drawable.SetVerticalTrack(object.skybox && data.vertical_tracking_skyboxes);

	if (object.collideable)
	{
		btTriangleIndexVertexArray * mesh = new btTriangleIndexVertexArray();
		mesh->addIndexedMesh(GetIndexedMesh(*object.model));
		data.meshes.push_back(mesh);

		assert(object.surface >= 0 && object.surface < (int)data.surfaces.size());
		btBvhTriangleMeshShape * shape = new btBvhTriangleMeshShape(mesh, true);
		shape->setUserPointer((void*)&data.surfaces[object.surface]);
		data.shapes.push_back(shape);

#ifndef EXTBULLET
		btTransform transform = btTransform::getIdentity();
		track_shape->addChildShape(transform, shape);
#else
		btCollisionObject * co = new btCollisionObject();
		co->setActivationState(DISABLE_SIMULATION);
		co->setCollisionShape(shape);
		co->setUserPointer(shape->getUserPointer());
		data.objects.push_back(co);
		world.addCollisionObject(co);
#endif
	}
	return true;
}

std::pair<bool, bool> TRACK::LOADER::ContinueOld()
{
	std::string model_name;
	if (!get(objectfile, model_name))
	{
		return std::make_pair(false, false);
	}

	OBJECT object;
	bool isashadow;
	std::string junk;

	get(objectfile, object.texture);
	get(objectfile, object.mipmap);
	get(objectfile, object.nolighting);
	get(objectfile, object.skybox);
	get(objectfile, object.transparent_blend);
	get(objectfile, junk);//bump_wavelength);
	get(objectfile, junk);//bump_amplitude);
	get(objectfile, junk);//driveable);
	get(objectfile, object.collideable);
	get(objectfile, junk);//friction_notread);
	get(objectfile, junk);//friction_tread);
	get(objectfile, junk);//rolling_resistance);
	get(objectfile, junk);//rolling_drag);
	get(objectfile, isashadow);
	get(objectfile, object.clamptexture);
	get(objectfile, object.surface);
	for (int i = 0; i < params_per_object - expected_params; i++)
	{
		get(objectfile, junk);
	}

	if (dynamic_shadows && isashadow)
	{
		return std::make_pair(false, true);
	}

	if (packload)
	{
		content.load(object.model, objectdir, model_name, pack);
	}
	else
	{
		content.load(object.model, objectdir, model_name);
	}

	if (agressive_combining)
	{
		std::map<std::string, OBJECT>::iterator i = combined.find(object.texture);
		if (i != combined.end() && !i->second.cached)
		{
			i->second.model->SetVertexArray(i->second.model->GetVertexArray() + object.model->GetVertexArray());
		}
		else
		{
			object.cached = content.get(object.model, objectdir, object.texture);
			combined[object.texture] = object;
		}
	}
	else
	{
		if (!AddObject(object))
		{
			return std::make_pair(true, false);
		}
	}

	return std::make_pair(false, true);
}

bool TRACK::LOADER::LoadSurfaces()
{
	std::string path = trackpath + "/surfaces.txt";
	std::ifstream file(path.c_str());
	if (!file.good())
	{
		info_output << "Can't find surfaces configfile: " << path << std::endl;
		return false;
	}

	PTree param;
	read_ini(file, param);
	for (PTree::const_iterator is = param.begin(); is != param.end(); ++is)
	{
		if (is->first.find("surface") != 0)
		{
			continue;
		}

		const PTree & surf_cfg = is->second;
		data.surfaces.push_back(TRACKSURFACE());
		TRACKSURFACE & surface = data.surfaces.back();

		std::string type;
		surf_cfg.get("Type", type);
		surface.setType(type);

		float temp = 0.0;
		surf_cfg.get("BumpWaveLength", temp, error_output);
		if (temp <= 0.0)
		{
			error_output << "Surface Type = " << type << " has BumpWaveLength = 0.0 in " << path << std::endl;
			temp = 1.0;
		}
		surface.bumpWaveLength = temp;

		surf_cfg.get("BumpAmplitude", temp, error_output);
		surface.bumpAmplitude = temp;

		surf_cfg.get("FrictionNonTread", temp, error_output);
		surface.frictionNonTread = temp;

		surf_cfg.get("FrictionTread", temp, error_output);
		surface.frictionTread = temp;

		surf_cfg.get("RollResistanceCoefficient", temp, error_output);
		surface.rollResistanceCoefficient = temp;

		surf_cfg.get("RollingDrag", temp, error_output);
		surface.rollingDrag = temp;
	}
	info_output << "Loaded surfaces file, " << data.surfaces.size() << " surfaces." << std::endl;

	return true;
}

bool TRACK::LOADER::LoadRoads()
{
	data.roads.clear();

	std::string roadpath = trackpath + "/roads.trk";
	std::ifstream trackfile(roadpath.c_str());
	if (!trackfile.good())
	{
		error_output << "Error opening roads file: " << trackpath + "/roads.trk" << std::endl;
		return false;
	}

	int numroads = 0;
	trackfile >> numroads;
	for (int i = 0; i < numroads && trackfile; ++i)
	{
		data.roads.push_back(ROADSTRIP());
		data.roads.back().ReadFrom(trackfile, data.reverse, error_output);
	}

	return true;
}

bool TRACK::LOADER::CreateRacingLines()
{
	TEXTUREINFO texinfo;
	content.load(data.racingline_texture, texturedir, "racingline.png", texinfo);

	K1999 k1999data;
	for (std::list <ROADSTRIP>::iterator i = data.roads.begin(); i != data.roads.end(); ++i)
	{
		if (k1999data.LoadData(*i))
		{
			k1999data.CalcRaceLine();
			k1999data.UpdateRoadStrip(*i);
		}
		//else error_output << "Couldn't create racing line for roadstrip " << n << std::endl;

		i->CreateRacingLine(data.racingline_node, data.racingline_texture);
	}

	return true;
}

bool TRACK::LOADER::LoadStartPositions(const PTree & info)
{
	int sp_num = 0;
	std::stringstream sp_name;
	sp_name << "start position " << sp_num;
	std::vector<float> f3(3);
	while (info.get(sp_name.str(), f3))
	{
		std::stringstream so_name;
		so_name << "start orientation " << sp_num;
		QUATERNION <float> q;
		std::vector <float> angle(3, 0.0);
		if (info.get(so_name.str(), angle, error_output))
		{
			q.SetEulerZYX(angle[0] * M_PI/180, angle[1] * M_PI/180, angle[2] * M_PI/180);
		}

		QUATERNION <float> orient(q[2], q[0], q[1], q[3]);

		//due to historical reasons the initial orientation places the car faces the wrong way
		QUATERNION <float> fixer;
		fixer.Rotate(M_PI_2, 0, 0, 1);
		orient = fixer * orient;

		MATHVECTOR <float, 3> pos(f3[2], f3[0], f3[1]);

		data.start_positions.push_back(
			std::pair <MATHVECTOR <float, 3>, QUATERNION <float> >(pos, orient));

		sp_num++;
		sp_name.str("");
		sp_name << "start position " << sp_num;
	}

	if (data.reverse)
	{
		// flip start positions
		for (std::vector <std::pair <MATHVECTOR <float, 3>, QUATERNION <float> > >::iterator i = data.start_positions.begin();
			i != data.start_positions.end(); ++i)
		{
			i->second.Rotate(M_PI, 0, 0, 1);
		}

		// reverse start positions
		std::reverse(data.start_positions.begin(), data.start_positions.end());
	}

	return true;
}

bool TRACK::LOADER::LoadLapSections(const PTree & info)
{
	// get timing sectors
	int lapmarkers = 0;
	if (info.get("lap sequences", lapmarkers))
	{
		for (int l = 0; l < lapmarkers; l++)
		{
			std::vector<float> lapraw(3);
			std::stringstream lapname;
			lapname << "lap sequence " << l;
			info.get(lapname.str(), lapraw);
			int roadid = lapraw[0];
			int patchid = lapraw[1];

			int curroad = 0;
			for (std::list<ROADSTRIP>::iterator i = data.roads.begin(); i != data.roads.end(); ++i, ++curroad)
			{
				if (curroad == roadid)
				{
					int num_patches = i->GetPatches().size();
					assert(patchid < num_patches);

					// adjust id for reverse case
					if (data.reverse)
						patchid = num_patches - patchid;

					data.lap.push_back(&i->GetPatches()[patchid].GetPatch());
					break;
				}
			}
		}
	}

	if (data.lap.empty())
	{
		info_output << "No lap sequence found. Lap timing will not be possible." << std::endl;
		return true;
	}

	// adjust timing sectors if reverse
	if (data.reverse)
	{
		if (data.lap.size() > 1)
		{
			// reverse the lap sequence, but keep the first bezier where it is (remember, the track is a loop)
			// so, for example, now instead of 1 2 3 4 we should have 1 4 3 2
			std::vector<const BEZIER *>::iterator secondbezier = data.lap.begin() + 1;
			assert(secondbezier != data.lap.end());
			std::reverse(secondbezier, data.lap.end());
		}

		// move timing sector 0 back so we'll still drive over it when going in reverse around the track
		// find patch in front of first start position
		const BEZIER * lap0 = 0;
		float minlen2 = 10E6;
		MATHVECTOR<float, 3> pos = data.start_positions[0].first;
		MATHVECTOR<float, 3> dir = direction::Forward;
		data.start_positions[0].second.RotateVector(dir);
		MATHVECTOR<float, 3> bpos(pos[1], pos[2], pos[0]);
		MATHVECTOR<float, 3> bdir(dir[1], dir[2], dir[0]);
		for (std::list<ROADSTRIP>::iterator r = data.roads.begin(); r != data.roads.end(); ++r)
		{
			for (std::vector<ROADPATCH>::iterator p = r->GetPatches().begin(); p != r->GetPatches().end(); ++p)
			{
				MATHVECTOR<float, 3> vec = p->GetPatch().GetBL() - bpos;
				float len2 = vec.MagnitudeSquared();
				bool fwd = vec.dot(bdir) > 0;
				if (fwd && len2 < minlen2)
				{
					minlen2 = len2;
					lap0 = &p->GetPatch();
				}
			}
		}
		if (lap0)
			data.lap[0] = lap0;
	}

	// calculate distance from starting line for each patch to account for those tracks
	// where starting line is not on the 1st patch of the road
	// note this only updates the road with lap sequence 0 on it
	BEZIER* start_patch = const_cast <BEZIER *> (data.lap[0]);
	start_patch->dist_from_start = 0.0;
	BEZIER* curr_patch = start_patch->next_patch;
	float total_dist = start_patch->length;
	int count = 0;
	while ( curr_patch && curr_patch != start_patch)
	{
		count++;
		curr_patch->dist_from_start = total_dist;
		total_dist += curr_patch->length;
		curr_patch = curr_patch->next_patch;
	}

	info_output << "Track timing sectors: " << lapmarkers << std::endl;
	return true;
}
