#include <GameLib/Scene/SceneObjectPropertiesLoader.h>
#include <GameLib/Scene/SceneObjectTypeNotFoundException.h>
#include <GameLib/Scene/SceneObjectVisitorException.h>
#include <GameLib/TypeRegistry.h>
#include <GameLib/TypeComplex.h>

#include <fmt/format.h>

#define NEXT_IP (++ip);
#define NEXT_OBJECT (++objectIdx);

namespace gamelib::scene
{
	using gamelib::prp::PRPOpCode;
	using gamelib::prp::PRPInstruction;
	using gamelib::scene::SceneObjectPropertiesLoader;

	struct InternalContext
	{
		uint32_t objectIdx = 0;

		void visitImpl(const SceneObject::Ptr& parent, const SceneObject::Ptr& currentObject, Span<SceneObject::Ptr> objects, Span<PRPInstruction> instructions);
	};

	void SceneObjectPropertiesLoader::load(Span<SceneObject::Ptr> objects, Span<PRPInstruction> instructions)
	{
		if (!objects || !instructions)
			return;

		InternalContext ctx;
		ctx.visitImpl(nullptr, objects[0], objects.slice(1, objects.size - 1), instructions);
	}

	void InternalContext::visitImpl(const SceneObject::Ptr &parent, const SceneObject::Ptr &currentObject, Span<SceneObject::Ptr> objects, Span<PRPInstruction> instructions)
	{
		/**
		 * Generic object declaration rules:
		 * 		Here we working with Glacier (R) object definition format. This format includes three sections:
		 * 		1) Object properties:
		 * 			[BeginObject|BeginNamedObject]
		 * 				(Properties)
		 * 			[EndObject]
		 * 		2) Controllers
		 * 			[Container - controllers count]
		 * 				[String - name]
		 * 				[BeginObject|BeginNamedObject]
		 * 					(Properties...)
		 * 				[EndObject]
		 * 		3) Children
		 * 			[Container - children count]
		 * 				[BeginObject|BeginNamedObject]
		 * 				<ZGEOM>
		 * 				[EndObject] ?
		 */

		/// ------------ STAGE 1: PROPERTIES ------------
		Span<PRPInstruction> ip = instructions;

		if (ip[0].getOpCode() != PRPOpCode::BeginObject && ip[0].getOpCode() != PRPOpCode::BeginNamedObject)
		{
			throw SceneObjectVisitorException(objectIdx, "Invalid object definition (expected BeginObject/BeginNamedObject)");
		}

		NEXT_IP

		// Check type
		const Type* objectType = TypeRegistry::getInstance().findTypeByHash(currentObject->getTypeId());
		if (!objectType)
		{
			throw SceneObjectTypeNotFoundException(objectIdx, currentObject->getTypeId());
		}

		// Read properties
		Value properties;
		{
			const auto& [vRes, _newInstructions] = objectType->verify(ip);
			if (!vRes)
			{
				throw SceneObjectVisitorException(objectIdx, "Invalid instructions set (verification failed)");
			}

			const auto& [value, newIP] = objectType->map(ip);

			if (!value.has_value())
			{
				throw SceneObjectVisitorException(objectIdx, "Invalid instructions set (verification failed) [2]");
			}

			properties = *value;
			ip = newIP; // Assign new ip
		}

		// Check that object ends with EndObject opcode
		if (ip[0].getOpCode() != PRPOpCode::EndObject)
		{
			throw SceneObjectVisitorException(objectIdx, "Object decl must ends with EndObject");
		}

		NEXT_IP

		/// ------------ STAGE 2: CONTROLLERS ------------
		if (ip[0].getOpCode() != PRPOpCode::Container && ip[0].getOpCode() == PRPOpCode::NamedContainer)
		{
			throw SceneObjectVisitorException(objectIdx, "Invalid object definition (Expected Container/NamedContainer)");
		}

		const auto controllersCount = ip[0].getOperand().trivial.i32;

		NEXT_IP

		std::map<std::string, Value> controllers;

		if (controllersCount > 0)
		{
			for (int32_t controllerIdx = 0; controllerIdx < controllersCount; ++controllerIdx)
			{
				if (ip[0].getOpCode() != PRPOpCode::String)
				{
					throw SceneObjectVisitorException(objectIdx, "Invalid controller definition (Expected String)");
				}

				const std::string& controllerName = ip[0].getOperand().str;

				NEXT_IP

				if (ip[0].getOpCode() != PRPOpCode::BeginObject && ip[0].getOpCode() != PRPOpCode::BeginNamedObject)
				{
					throw SceneObjectVisitorException(objectIdx, "Invalid controller definition (Expected BeginObject/BeginNamedObject)");
				}

				NEXT_IP

				// Find type
				const Type* controllerType = TypeRegistry::getInstance().findTypeByShortName(controllerName);
				if (!controllerType)
				{
					throw SceneObjectTypeNotFoundException(objectIdx, controllerName);
				}

				if (controllerType->getKind() != TypeKind::COMPLEX)
				{
					// Only complex types are allowed to be controllers
					throw SceneObjectVisitorException(objectIdx, fmt::format("Type '{}' not allowed to be controller because it's not COMPLEX"));
				}

				// Map controller properties
				const auto& [controllerMapResult, nextIP] = controllerType->map(ip);

				if (!controllerMapResult.has_value())
				{
					throw SceneObjectVisitorException(objectIdx, "Failed to map controller");
				}

				ip = nextIP;

				controllers[controllerName] = controllerMapResult.value();

				if (ip[0].getOpCode() != PRPOpCode::EndObject && reinterpret_cast<const TypeComplex*>(controllerType)->areUnexposedInstructionsAllowed())
				{
					// Here we need to extract all instructions until 'EndObject'
					Span<PRPInstruction> begin = ip, end = ip;
					int64_t endOffset = 0;

					// Find nearest 'EndObject' instruction and instruction before it will be our last instruction
					while (!end.empty() && end[0].getOpCode() != PRPOpCode::EndObject)
					{
						end = end.slice(1, end.size - 1);
						++endOffset;
					}

					if (end.empty())
					{
						throw SceneObjectVisitorException(objectIdx, fmt::format("Invalid controller definition: We have controller '{}' with unexposed instructions and without EndObject instruction!", controllerName));
					}

					auto unexposedInstructions = begin.slice(0, ip.size - endOffset).as<std::vector<PRPInstruction>>();

					auto& orgInstructionsRef = controllers[controllerName].getInstructions();
					orgInstructionsRef.insert(orgInstructionsRef.end(), unexposedInstructions.begin(), unexposedInstructions.end());

					ip = ip.slice(endOffset, ip.size - endOffset);
				}

				if (ip[0].getOpCode() != PRPOpCode::EndObject)
				{
					throw SceneObjectVisitorException(objectIdx, "Invalid controller definition (Expected EndObject)");
				}

				NEXT_IP
			}
		}

		currentObject->getControllers() = controllers;
		currentObject->getProperties()  = properties;

		/// ------------ STAGE 3: CHILDREN ------------
		/*
		 * 		3) Children
		 * 			[Container - children count]
		 * 				[BeginObject|BeginNamedObject]
		 * 				<ZGEOM>
		 * 				[EndObject] ?
		 */
		if (ip[0].getOpCode() != PRPOpCode::Container && ip[0].getOpCode() != PRPOpCode::NamedContainer)
		{
			throw SceneObjectVisitorException(objectIdx, "Invalid controller definition (Expected Container with children geoms)");
		}

		const int32_t childrenCount = ip[0].getOperand().trivial.i32;

		NEXT_IP

		for (int childrenGeomIdx = 0; childrenGeomIdx < childrenCount; ++childrenGeomIdx)
		{
			if (parent)
			{
				currentObject->setParent(parent);
			}

			NEXT_OBJECT
			visitImpl(currentObject, objects[0], objects.slice(1, objects.size - 1), ip);

			if (ip[0].getOpCode() != PRPOpCode::EndObject)
			{
				throw SceneObjectVisitorException(objectIdx, "Invalid children definition (Expected EndObject)");
			}

			NEXT_IP
		}
	}
}

#undef NEXT_IP
#undef NEXT_OBJECT