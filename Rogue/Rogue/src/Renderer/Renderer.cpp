#include "Renderer.h"
#include "GBuffer.h"
#include "PresentFrameBuffer.h"
#include "Mesh.h"
#include "MeshUtil.hpp"
#include "Shader.h"
#include "ShadowMap.h"
#include "Texture.h"
#include "Texture3D.h"
#include "../common.h"
#include "../Util.hpp"
#include "../Core/Audio.hpp"
#include "../Core/GL.h"
#include "../Core/Physics.h"
#include "../Core/Player.h"
#include "../Core/Input.h"
#include "../Core/Scene.h"
#include "../Core/AssetManager.h"
#include "../Core/TextBlitter.h"
#include "../Core/Editor.h"
#include "Model.h"
#include "NumberBlitter.h"
#include "../Timer.hpp"

#include <vector>
#include <cstdlib>
#include <format>
#include <future>
#include <algorithm>

#include "../Core/AnimatedGameObject.h"
#include "../Effects/MuzzleFlash.h"

std::vector<glm::vec3> debugPoints;



struct Shaders {
    Shader solidColor;
    Shader shadowMap;
    Shader UI;
    Shader editorSolidColor;
    Shader editorTextured;
    Shader composite;
    Shader fxaa;
    Shader animatedQuad;
    Shader depthOfField;
    Shader debugViewPointCloud;
    Shader debugViewPropgationGrid;
    Shader geometry;
    Shader geometry_instanced;
    Shader lighting;
    Shader bulletDecals;
    ComputeShader compute;
    ComputeShader pointCloud;
    ComputeShader propogateLight;
    //ComputeShader propogationList;
    //ComputeShader calculateIndirectDispatchSize;
} _shaders;

struct SSBOs {
    GLuint rtVertices = 0;
    GLuint rtMesh = 0;
    GLuint rtInstances = 0;
    GLuint dirtyPointCloudIndices = 0;
    //GLuint dirtyGridChunks = 0; // 4x4x4
   // GLuint atomicCounter = 0;
    //GLuint propogationList = 0; // contains coords of all dirty grid points
   // GLuint indirectDispatchSize = 0;
   // GLuint floorVertices = 0;
    GLuint dirtyGridCoordinates = 0;
    GLuint instanceMatrices = 0;
} _ssbos;

struct PointCloud {
    GLuint VAO{ 0 };
    GLuint VBO{ 0 };
    int vertexCount{ 0 };
} _pointCloud;

struct Toggles {
    bool drawLights = true;
    bool drawProbes = false;
    bool drawLines = false;
    //bool drawPhysXWorld = false;
    bool drawCollisionWorld = false;
} _toggles;

struct PlayerRenderTarget {
    GBuffer gBuffer;
    PresentFrameBuffer presentFrameBuffer;
};

GLuint _progogationGridTexture = 0;
Mesh _cubeMesh;
unsigned int _pointLineVAO;
unsigned int _pointLineVBO;
int _renderWidth = 768;// 512 * 1.5f;
int _renderHeight = 432;// 288 * 1.5f;

std::vector<PlayerRenderTarget> _playerRenderTargets;
std::vector<Point> _points;
std::vector<Point> _solidTrianglePoints;
std::vector<Line> _lines;
std::vector<UIRenderInfo> _UIRenderInfos;
std::vector<ShadowMap> _shadowMaps;
GLuint _imageStoreTexture = { 0 };
bool _depthOfFieldScene = 0.9f;
bool _depthOfFieldWeapon = 1.0f;
const float _propogationGridSpacing = 0.4f;
const float _pointCloudSpacing = 0.4f;
const float _maxPropogationDistance = 2.6f; 
const float _maxDistanceSquared = _maxPropogationDistance * _maxPropogationDistance;
float _mapWidth = 16;
float _mapHeight = 8;
float _mapDepth = 16; 
//std::vector<int> _newDirtyPointCloudIndices;
int _floorVertexCount;

const int _gridTextureWidth = (int)(_mapWidth / _propogationGridSpacing);
const int _gridTextureHeight = (int)(_mapHeight / _propogationGridSpacing);
const int _gridTextureDepth = (int)(_mapDepth / _propogationGridSpacing);
const int _gridTextureSize = (int)(_gridTextureWidth * _gridTextureHeight * _gridTextureDepth);
//const int _xSize = std::ceil(_gridTextureWidth * 0.25f);
//const int _ySize = std::ceil(_gridTextureHeight * 0.25f);
//const int _zSize = std::ceil(_gridTextureDepth * 0.25f);

//std::vector<glm::uvec4> _gridIndices;
//std::vector<glm::uvec4> _newGridIndices;
static std::vector<glm::uvec4> _probeCoordsWithinMapBounds;
static std::vector<glm::vec3> _probeWorldPositionsWithinMapBounds;
std::vector<glm::uvec4> _dirtyProbeCoords;
std::vector<int> _dirtyPointCloudIndices;
int _dirtyProbeCount;

std::vector<glm::mat4> _glockMatrices;
std::vector<glm::mat4> _aks74uMatrices;


enum RenderMode { COMPOSITE, DIRECT_LIGHT, INDIRECT_LIGHT, POINT_CLOUD, MODE_COUNT } _mode;
enum DebugLineRenderMode { SHOW_NO_LINES, PHYSX_ALL, PHYSX_RAYCAST, PHYSX_COLLISION, RAYTRACE_LAND, DEBUG_LINE_MODE_COUNT} _debugLineRenderMode;

void DrawScene(Shader& shader);
void DrawAnimatedScene(Shader& shader, Player* player);
void DrawShadowMapScene(Shader& shader);
void DrawBulletDecals(Player* player);
void DrawCasingProjectiles(Player* player);
void DrawFullscreenQuad();
void DrawMuzzleFlashes(Player* player);
void InitCompute();
void ComputePass();
void RenderShadowMaps();
void UpdatePointCloudLighting();
void UpdatePropogationgGrid();
void DrawPointCloud(Player* player);
void GeometryPass(Player* player);
void LightingPass(Player* player);
void DebugPass(Player* player);
void RenderImmediate();
void FindProbeCoordsWithinMapBounds();
void CalculateDirtyCloudPoints();
void CalculateDirtyProbeCoords();
//std::vector<glm::uvec4> GridIndicesUpdate(std::vector<glm::uvec4> allGridIndicesWithinRooms);

void Renderer::Init() {

    glGenVertexArrays(1, &_pointLineVAO);
    glGenBuffers(1, &_pointLineVBO);
    glPointSize(2);
        
    _shaders.solidColor.Load("solid_color.vert", "solid_color.frag");
    _shaders.shadowMap.Load("shadowmap.vert", "shadowmap.frag", "shadowmap.geom");
    _shaders.UI.Load("ui.vert", "ui.frag");
    _shaders.editorSolidColor.Load("editor_solid_color.vert", "editor_solid_color.frag");
    _shaders.composite.Load("composite.vert", "composite.frag");
    _shaders.fxaa.Load("fxaa.vert", "fxaa.frag");
    _shaders.animatedQuad.Load("animated_quad.vert", "animated_quad.frag");
    _shaders.depthOfField.Load("depth_of_field.vert", "depth_of_field.frag");
    _shaders.debugViewPointCloud.Load("debug_view_point_cloud.vert", "debug_view_point_cloud.frag");
    _shaders.geometry.Load("geometry.vert", "geometry.frag");
    _shaders.geometry_instanced.Load("geometry_instanced.vert", "geometry_instanced.frag");
    _shaders.lighting.Load("lighting.vert", "lighting.frag");
    _shaders.debugViewPropgationGrid.Load("debug_view_propogation_grid.vert", "debug_view_propogation_grid.frag");
    _shaders.editorTextured.Load("editor_textured.vert", "editor_textured.frag");
    _shaders.bulletDecals.Load("bullet_decals.vert", "bullet_decals.frag");
    

    _cubeMesh = MeshUtil::CreateCube(1.0f, 1.0f, true);

    RecreateFrameBuffers();



    /*
    _shadowMaps.push_back(ShadowMap());
    _shadowMaps.push_back(ShadowMap());
    _shadowMaps.push_back(ShadowMap());
    _shadowMaps.push_back(ShadowMap());
    */

    for(int i = 0; i < 16; i++) {
        _shadowMaps.emplace_back();
    }

    for (ShadowMap& shadowMap : _shadowMaps) {
        shadowMap.Init();
    }

    FindProbeCoordsWithinMapBounds();

    InitCompute();
}

int GetPlayerIndexFromPlayerPointer(Player* player) {
    for (int i = 0; i < Scene::_players.size(); i++) {
        if (&Scene::_players[i] == player) {
            return i;
        }
    }
    return -1;
}

PlayerRenderTarget& GetPlayerRenderTarget(int playerIndex) {
    return _playerRenderTargets[playerIndex];
}

void Renderer::RenderFrame(Player* player) {

    if (!player) {
        return;
    }

    int playerIndex = GetPlayerIndexFromPlayerPointer(player);

    if (playerIndex == -1) {
        return;
    }

    PlayerRenderTarget& playerRenderTarget = GetPlayerRenderTarget(playerIndex);
    GBuffer& gBuffer = playerRenderTarget.gBuffer;
    PresentFrameBuffer& presentFrameBuffer = playerRenderTarget.presentFrameBuffer;

    if (playerIndex == 0) {
        RenderShadowMaps();
        CalculateDirtyCloudPoints();
        CalculateDirtyProbeCoords();
        ComputePass(); // Fills the indirect lighting data structures
    }

    GeometryPass(player);
    DrawBulletDecals(player);
    DrawCasingProjectiles(player);
    LightingPass(player);
    DrawMuzzleFlashes(player);
    
    // Propgation Grid
    if (_toggles.drawProbes) {
        Transform cubeTransform;
        cubeTransform.scale = glm::vec3(0.025f);
        _shaders.debugViewPropgationGrid.Use();
        _shaders.debugViewPropgationGrid.SetMat4("projection", GetProjectionMatrix(_depthOfFieldScene));
        _shaders.debugViewPropgationGrid.SetMat4("view", player->GetViewMatrix());
        _shaders.debugViewPropgationGrid.SetMat4("model", cubeTransform.to_mat4());
        _shaders.debugViewPropgationGrid.SetFloat("propogationGridSpacing", _propogationGridSpacing);
        _shaders.debugViewPropgationGrid.SetInt("propogationTextureWidth", _gridTextureWidth);
        _shaders.debugViewPropgationGrid.SetInt("propogationTextureHeight", _gridTextureHeight);
        _shaders.debugViewPropgationGrid.SetInt("propogationTextureDepth", _gridTextureDepth);
        int count = _gridTextureWidth * _gridTextureHeight * _gridTextureDepth;
        glEnable(GL_CULL_FACE);
        glEnable(GL_DEPTH_TEST);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_3D, _progogationGridTexture);
        glBindVertexArray(_cubeMesh.GetVAO());
        glDrawElementsInstanced(GL_TRIANGLES, _cubeMesh.GetIndexCount(), GL_UNSIGNED_INT, 0, count);
    }

    // Blit final image from large FBO down into a smaller FBO
    glBindFramebuffer(GL_READ_FRAMEBUFFER, gBuffer.GetID());
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, presentFrameBuffer.GetID());
    glReadBuffer(GL_COLOR_ATTACHMENT3);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glBlitFramebuffer(0, 0, gBuffer.GetWidth(), gBuffer.GetHeight(), 0, 0, presentFrameBuffer.GetWidth(), presentFrameBuffer.GetHeight(), GL_COLOR_BUFFER_BIT, GL_LINEAR);

    // FXAA on that smaller FBO
    _shaders.fxaa.Use();
    _shaders.fxaa.SetFloat("viewportWidth", presentFrameBuffer.GetWidth());
    _shaders.fxaa.SetFloat("viewportHeight", presentFrameBuffer.GetHeight());
    presentFrameBuffer.Bind();
    glViewport(0, 0, presentFrameBuffer.GetWidth(), presentFrameBuffer.GetHeight());
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, presentFrameBuffer._inputTexture);
    glDrawBuffer(GL_COLOR_ATTACHMENT1);
    glDisable(GL_DEPTH_TEST);
    DrawFullscreenQuad();

    // Render any debug shit, like the point cloud, progation grid, misc points, lines, etc
    DebugPass(player);

    // Render UI
    presentFrameBuffer.Bind();
    glDrawBuffer(GL_COLOR_ATTACHMENT1);
    std::string texture = "CrosshairDot";
    if (player->CursorShouldBeInterect()) {
        texture = "CrosshairSquare";
    }
    QueueUIForRendering(texture, presentFrameBuffer.GetWidth() / 2, presentFrameBuffer.GetHeight() / 2, true);
   
    /*if (Scene::_cameraRayData.found) {
        if (Scene::_cameraRayData.raycastObjectType == RaycastObjectType::WALLS) {
            TextBlitter::_debugTextToBilt += "Ray hit: WALL\n";
        }
        if (Scene::_cameraRayData.raycastObjectType == RaycastObjectType::DOOR) {
            TextBlitter::_debugTextToBilt += "Ray hit: DOOR\n";
        }
    }
    else {
        TextBlitter::_debugTextToBilt += "Ray hit: NONE\n";
    }
    */

    TextBlitter::_debugTextToBilt += "View pos: " + Util::Vec3ToString(player->GetViewPos()) + "\n";
    TextBlitter::_debugTextToBilt += "View rot: " + Util::Vec3ToString(player->GetViewRotation()) + "\n";
    TextBlitter::_debugTextToBilt = "";
    glBindFramebuffer(GL_FRAMEBUFFER, presentFrameBuffer.GetID());
    glViewport(0, 0, presentFrameBuffer.GetWidth(), presentFrameBuffer.GetHeight());
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    Renderer::RenderUI();

    // Blit that smaller FBO into the main frame buffer 
    if (_viewportMode == FULLSCREEN) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, playerRenderTarget.presentFrameBuffer.GetID());
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glReadBuffer(GL_COLOR_ATTACHMENT1);
        glBlitFramebuffer(0, 0, playerRenderTarget.presentFrameBuffer.GetWidth(), playerRenderTarget.presentFrameBuffer.GetHeight(), 0, 0, GL::GetWindowWidth(), GL::GetWindowHeight(), GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    else if (_viewportMode == SPLITSCREEN) {

        if (GetPlayerIndexFromPlayerPointer(player) == 0) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, playerRenderTarget.presentFrameBuffer.GetID());
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            glReadBuffer(GL_COLOR_ATTACHMENT1);
            glBlitFramebuffer(0, 0, playerRenderTarget.presentFrameBuffer.GetWidth(), playerRenderTarget.presentFrameBuffer.GetHeight(), 0, GL::GetWindowHeight() / 2, GL::GetWindowWidth(), GL::GetWindowHeight(), GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        if (GetPlayerIndexFromPlayerPointer(player) == 1) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, playerRenderTarget.presentFrameBuffer.GetID());
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            glReadBuffer(GL_COLOR_ATTACHMENT1);
            glBlitFramebuffer(0, 0, playerRenderTarget.presentFrameBuffer.GetWidth(), playerRenderTarget.presentFrameBuffer.GetHeight(), 0, 0, GL::GetWindowWidth(), GL::GetWindowHeight() / 2, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
    }

    // Wipe all light dirty flags back to false
    for (Light& light : Scene::_lights) {
        light.isDirty = false;
    }
}

void GeometryPass(Player* player) {
    glm::mat4 projection = Renderer::GetProjectionMatrix(_depthOfFieldScene); // 1.0 for weapon, 0.9 for scene.
    glm::mat4 view = player->GetViewMatrix();
    
    int playerIndex = GetPlayerIndexFromPlayerPointer(player);
    PlayerRenderTarget& playerRenderTarget = GetPlayerRenderTarget(playerIndex);
    GBuffer& gBuffer = playerRenderTarget.gBuffer;

    gBuffer.Bind();
    unsigned int attachments[3] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
    glDrawBuffers(3, attachments);
    glViewport(0, 0, gBuffer.GetWidth(), gBuffer.GetHeight());
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    _shaders.geometry.Use();
    _shaders.geometry.SetMat4("projection", projection);
    _shaders.geometry.SetMat4("view", view);
    _shaders.geometry.SetVec3("viewPos", player->GetViewPos());
    _shaders.geometry.SetVec3("camForward", player->GetCameraForward());
    DrawAnimatedScene(_shaders.geometry, player);
    DrawScene(_shaders.geometry);
}

void LightingPass(Player* player) {

    int playerIndex = GetPlayerIndexFromPlayerPointer(player);
    PlayerRenderTarget& playerRenderTarget = GetPlayerRenderTarget(playerIndex);
    GBuffer& gBuffer = playerRenderTarget.gBuffer;

    _shaders.lighting.Use();

    // Debug Text
    TextBlitter::_debugTextToBilt = "";
    if (_mode == RenderMode::COMPOSITE) {
        TextBlitter::_debugTextToBilt = "Mode: COMPOSITE\n";
        _shaders.lighting.SetInt("mode", 0);
    }
    if (_mode == RenderMode::POINT_CLOUD) {
        TextBlitter::_debugTextToBilt = "Mode: POINT CLOUD\n";
        _shaders.lighting.SetInt("mode", 1);
    }
    if (_mode == RenderMode::DIRECT_LIGHT) {
        TextBlitter::_debugTextToBilt = "Mode: DIRECT LIGHT\n";
        _shaders.lighting.SetInt("mode", 2);
    }
    if (_mode == RenderMode::INDIRECT_LIGHT) {
        TextBlitter::_debugTextToBilt = "Mode: INDIRECT LIGHT\n";
        _shaders.lighting.SetInt("mode", 3);
    }

    if (_debugLineRenderMode == DebugLineRenderMode::SHOW_NO_LINES) {
        // Do nothing
    }
    else if (_debugLineRenderMode == DebugLineRenderMode::PHYSX_ALL) {
        TextBlitter::_debugTextToBilt += "Line Mode: PHYSX ALL\n";
    }
    else if (_debugLineRenderMode == DebugLineRenderMode::PHYSX_RAYCAST) {
        TextBlitter::_debugTextToBilt += "Line Mode: PHYSX RAYCAST\n";
    }
    else if (_debugLineRenderMode == DebugLineRenderMode::PHYSX_COLLISION) {
        TextBlitter::_debugTextToBilt += "Line Mode: PHYSX COLLISION\n";
    }
    else if (_debugLineRenderMode == DebugLineRenderMode::RAYTRACE_LAND) {
        TextBlitter::_debugTextToBilt += "Line Mode: COMPUTE RAY WORLD\n";
    }
  
    gBuffer.Bind();
    glDrawBuffer(GL_COLOR_ATTACHMENT3);
    glViewport(0, 0, gBuffer.GetWidth(), gBuffer.GetHeight());
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gBuffer._gBaseColorTexture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, gBuffer._gNormalTexture);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, gBuffer._gRMATexture);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, gBuffer._gDepthTexture);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_3D, _progogationGridTexture);

    // Update lights
    auto& lights = Scene::_lights;
    for (int i = 0; i < lights.size(); i++) {
        if (i >= 16) break;
        glActiveTexture(GL_TEXTURE5 + i);
        glBindTexture(GL_TEXTURE_CUBE_MAP, _shadowMaps[i]._depthTexture);
        _shaders.lighting.SetVec3("lights[" + std::to_string(i) + "].position", lights[i].position);
        _shaders.lighting.SetVec3("lights[" + std::to_string(i) + "].color", lights[i].color);
        _shaders.lighting.SetFloat("lights[" + std::to_string(i) + "].radius", lights[i].radius);
        _shaders.lighting.SetFloat("lights[" + std::to_string(i) + "].strength", lights[i].strength);
    }
    _shaders.lighting.SetInt("lightsCount", std::min((int)lights.size(), 16));

    static float time = 0;
    time += 0.01f; 
    _shaders.lighting.SetMat4("model", glm::mat4(1));
    _shaders.lighting.SetFloat("time", time);
    _shaders.lighting.SetFloat("screenWidth", gBuffer.GetWidth());
    _shaders.lighting.SetFloat("screenHeight", gBuffer.GetHeight());
    _shaders.lighting.SetMat4("projectionScene", Renderer::GetProjectionMatrix(_depthOfFieldScene));
    _shaders.lighting.SetMat4("projectionWeapon", Renderer::GetProjectionMatrix(_depthOfFieldWeapon));
    _shaders.lighting.SetMat4("inverseProjectionScene", glm::inverse(Renderer::GetProjectionMatrix(_depthOfFieldScene)));
    _shaders.lighting.SetMat4("inverseProjectionWeapon", glm::inverse(Renderer::GetProjectionMatrix(_depthOfFieldWeapon)));
    _shaders.lighting.SetMat4("view", player->GetViewMatrix());
    _shaders.lighting.SetMat4("inverseView", glm::inverse(player->GetViewMatrix()));
    _shaders.lighting.SetVec3("viewPos", player->GetViewPos());
    _shaders.lighting.SetFloat("propogationGridSpacing", _propogationGridSpacing);

    DrawFullscreenQuad();
}

void DebugPass(Player* player) {

    int playerIndex = GetPlayerIndexFromPlayerPointer(player);
    PlayerRenderTarget& playerRenderTarget = GetPlayerRenderTarget(playerIndex);
    //GBuffer& gBuffer = playerRenderTarget.gBuffer;
    PresentFrameBuffer& presentFrameBuffer = playerRenderTarget.presentFrameBuffer;

    presentFrameBuffer.Bind();
    glDrawBuffer(GL_COLOR_ATTACHMENT1);
    glViewport(0, 0, presentFrameBuffer.GetWidth(), presentFrameBuffer.GetHeight());
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    // Point cloud
    if (_mode == RenderMode::POINT_CLOUD) {
        DrawPointCloud(player);
    }

    _shaders.solidColor.Use();
    _shaders.solidColor.SetMat4("projection", Renderer::GetProjectionMatrix(_depthOfFieldScene));
    _shaders.solidColor.SetMat4("view", player->GetViewMatrix());
    _shaders.solidColor.SetVec3("viewPos", player->GetViewPos());
    _shaders.solidColor.SetVec3("color", glm::vec3(1, 1, 0));

    // Lights
    if (_toggles.drawLights) {
        //std::cout << Scene::_lights.size() << "\n";
        for (Light& light : Scene::_lights) {
            glm::vec3 lightCenter = light.position;
            Transform lightTransform;
            lightTransform.scale = glm::vec3(0.2f);
            lightTransform.position = lightCenter;
            _shaders.solidColor.SetMat4("model", lightTransform.to_mat4());

            if (light.isDirty) {
                _shaders.solidColor.SetVec3("color", YELLOW);
            }
            else {
                _shaders.solidColor.SetVec3("color", WHITE);
            }
            _shaders.solidColor.SetBool("uniformColor", true);
            _cubeMesh.Draw();
        }
    }

    // Draw casings  
      _points.clear();
      _shaders.solidColor.Use();
      _shaders.solidColor.SetBool("uniformColor", false);
      _shaders.solidColor.SetMat4("model", glm::mat4(1));
      /*for (auto& casing : Scene::_bulletCasings) {
          Point point;
          //point.pos = casing.position;
          point.color = LIGHT_BLUE;
         // Renderer::QueuePointForDrawing(point);
      }*/
      glDisable(GL_DEPTH_TEST);
      glDisable(GL_CULL_FACE);
      RenderImmediate();
      

    _points.clear();
    _shaders.solidColor.Use();
    _shaders.solidColor.SetBool("uniformColor", false);
    _shaders.solidColor.SetMat4("model", glm::mat4(1));



    for (auto& pos : debugPoints) {
        Point point;
        point.pos = pos;
        point.color = LIGHT_BLUE;
        Renderer::QueuePointForDrawing(point);
    }
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    RenderImmediate();
    debugPoints.clear();
    


    // Draw physx word as lines
  /*  _lines.clear();
    _shaders.solidColor.Use();
    _shaders.solidColor.SetBool("uniformColor", false);
    _shaders.solidColor.SetMat4("model", glm::mat4(1));
    auto* physics = Physics::GetPhysics();
    auto* scene = Physics::GetScene();
    auto& renderBuffer = scene->getRenderBuffer();
    for (int i = 0; i < renderBuffer.getNbLines(); i++) {
        auto pxLine = renderBuffer.getLines()[i];
        Line line;
        line.p1.pos = Util::PxVec3toGlmVec3(pxLine.pos0);
        line.p2.pos = Util::PxVec3toGlmVec3(pxLine.pos1);
        line.p1.color = GREEN;
        line.p2.color = GREEN;
        //Renderer::QueueLineForDrawing(line);
    }

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    RenderImmediate();
    */
    // Draw debugcals
   /*
    _points.clear();
    _shaders.solidColor.Use();
    _shaders.solidColor.SetBool("uniformColor", false);
    _shaders.solidColor.SetMat4("model", glm::mat4(1));
    for (auto& decal : Scene::_decals) {

        glm::mat4 parentMatrix = Util::PxMat44ToGlmMat4(decal.parent->getGlobalPose());

        glm::mat4 modelMatrix = decal.GetModelMatrix();
        float x = modelMatrix[3][0];
        float y = modelMatrix[3][1];
        float z = modelMatrix[3][2];

        //glm::vec3 worldPosition = parentMatrix * glm::vec4(decal.position, 1.0);

        Point point;
        point.pos = glm::vec3(x, y, z);
        point.color = LIGHT_BLUE;
        Renderer::QueuePointForDrawing(point);
    }
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    RenderImmediate();
    ;*/



    glm::vec3 color = GREEN;
    PxScene* scene = Physics::GetScene();
    PxU32 nbActors = scene->getNbActors(PxActorTypeFlag::eRIGID_DYNAMIC | PxActorTypeFlag::eRIGID_STATIC);
    if (nbActors) {
        std::vector<PxRigidActor*> actors(nbActors);
        scene->getActors(PxActorTypeFlag::eRIGID_DYNAMIC | PxActorTypeFlag::eRIGID_STATIC, reinterpret_cast<PxActor**>(&actors[0]), nbActors);
        for (PxRigidActor* actor : actors) {
            PxShape* shape;
            actor->getShapes(&shape, 1);
            actor->setActorFlag(PxActorFlag::eVISUALIZATION, true);

            if (_debugLineRenderMode == DebugLineRenderMode::PHYSX_RAYCAST) {
                color = RED;
                if (shape->getQueryFilterData().word0 == RaycastGroup::RAYCAST_DISABLED) {
                    actor->setActorFlag(PxActorFlag::eVISUALIZATION, false);
                }
            }    
            else if (_debugLineRenderMode == DebugLineRenderMode::PHYSX_COLLISION) {
                color = LIGHT_BLUE;
                if (shape->getQueryFilterData().word1 == CollisionGroup::NO_COLLISION) {
                    actor->setActorFlag(PxActorFlag::eVISUALIZATION, false);
                } 
            }
        }
    }


        // Debug lines

        if (_debugLineRenderMode == DebugLineRenderMode::PHYSX_ALL ||
            _debugLineRenderMode == DebugLineRenderMode::PHYSX_COLLISION ||
            _debugLineRenderMode == DebugLineRenderMode::PHYSX_RAYCAST) {


/*
        //    Physics::EnableRigidBodyDebugLines(Scene::_sceneRigidDynamic);

            for (BulletCasing& casing : Scene::_bulletCasings) {
                if (casing.HasActivePhysics()) {
                 //   Physics::EnableRigidBodyDebugLines(casing.rigidBody);
                }
            }
            for (Door& door : Scene::_doors) {
                Physics::EnableRigidBodyDebugLines(door.collisionBody);
                //Physics::EnableRigidBodyDebugLines(door.raycastBody);
            }
            glm::vec3 color = GREEN;

            if (_debugLineRenderMode == DebugLineRenderMode::PHYSX_COLLISION) {
                color = LIGHT_BLUE;
                for (BulletCasing& casing : Scene::_bulletCasings) {

                }
                for (Door& door : Scene::_doors) {
                    //Physics::DisableRigidBodyDebugLines(door.raycastBody);
                }
            }
            else if (_debugLineRenderMode == DebugLineRenderMode::PHYSX_RAYCAST) {
                
                for (BulletCasing& casing : Scene::_bulletCasings) {
                    if (casing.HasActivePhysics()) {
                  //      Physics::DisableRigidBodyDebugLines(casing.rigidBody);
                    }
                }
                for (Door& door : Scene::_doors) {
                    Physics::DisableRigidBodyDebugLines(door.collisionBody);
                }
            }
            */
            _lines.clear();
            _shaders.solidColor.Use();
            _shaders.solidColor.SetBool("uniformColor", false);
            _shaders.solidColor.SetMat4("model", glm::mat4(1));
            auto& renderBuffer = scene->getRenderBuffer();
            for (unsigned int i = 0; i < renderBuffer.getNbLines(); i++) {
                auto pxLine = renderBuffer.getLines()[i];
                Line line;
                line.p1.pos = Util::PxVec3toGlmVec3(pxLine.pos0);
                line.p2.pos = Util::PxVec3toGlmVec3(pxLine.pos1);
                line.p1.color = color;
                line.p2.color = color;
                Renderer::QueueLineForDrawing(line);
            }
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            RenderImmediate();
        }
        else if (_debugLineRenderMode == RAYTRACE_LAND) {
            _shaders.solidColor.Use();
            _shaders.solidColor.SetBool("uniformColor", true);
            _shaders.solidColor.SetVec3("color", YELLOW);
            _shaders.solidColor.SetMat4("model", glm::mat4(1));
            for (RTInstance& instance : Scene::_rtInstances) {
                RTMesh& mesh = Scene::_rtMesh[instance.meshIndex];

                for (unsigned int i = mesh.baseVertex; i < mesh.baseVertex + mesh.vertexCount; i+=3) {
                    Triangle t;
                    t.p1 = instance.modelMatrix * glm::vec4(Scene::_rtVertices[i+0], 1.0);
                    t.p2 = instance.modelMatrix * glm::vec4(Scene::_rtVertices[i+1], 1.0);
                    t.p3 = instance.modelMatrix * glm::vec4(Scene::_rtVertices[i+2], 1.0);
                    t.color = YELLOW;
                    Renderer::QueueTriangleForLineRendering(t);
                }
            }
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            RenderImmediate();
        }    

    // Draw collision world
    if (_toggles.drawCollisionWorld) {
        _shaders.solidColor.Use();
        _shaders.solidColor.SetBool("uniformColor", false);
        _shaders.solidColor.SetMat4("model", glm::mat4(1));

        for (Line& collisionLine : Scene::_collisionLines) {
            Renderer::QueueLineForDrawing(collisionLine);
        }

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        RenderImmediate();

        // Draw player sphere
        auto* sphere = AssetManager::GetModel("Sphere");
        Transform transform;
        transform.position = player->GetFeetPosition() + glm::vec3(0, 0.101f, 0);
        transform.scale.x = player->GetRadius();
        transform.scale.y = 0.0f;
        transform.scale.z = player->GetRadius();
        _shaders.solidColor.SetBool("uniformColor", true);
        _shaders.solidColor.SetVec3("color", LIGHT_BLUE);
        _shaders.solidColor.SetMat4("model", transform.to_mat4());
        sphere->Draw();
    }

}

void Renderer::RecreateFrameBuffers() {

    float width = (float)_renderWidth;
    float height = (float)_renderHeight;
    int playerCount = Scene::_playerCount;

    // Adjust for splitscreen
    if (_viewportMode == SPLITSCREEN) {
        height *= 0.5f;
    }

    // Cleanup any existing player render targest
    for (PlayerRenderTarget& playerRenderTarget : _playerRenderTargets) {
        playerRenderTarget.gBuffer.Destroy();
        playerRenderTarget.presentFrameBuffer.Destroy();
    }
    _playerRenderTargets.clear();

    // Create a PlayerRenderTarget for each player
    for (int i = 0; i < playerCount; i++) {
        PlayerRenderTarget& playerRenderTarget = _playerRenderTargets.emplace_back(PlayerRenderTarget());
        playerRenderTarget.gBuffer.Configure(width * 2, height * 2);
        playerRenderTarget.presentFrameBuffer.Configure(width, height);
        
        if (_viewportMode == FULLSCREEN) {
            break;
        }
    }

    std::cout << "Render Size: " << width * 2 << " x " << height * 2 << "\n";
    std::cout << "Present Size: " << width << " x " << height << "\n";
    std::cout << "PlayerRenderTarget Count: " << _playerRenderTargets.size() << "\n";
}

void DrawScene(Shader& shader) {

    shader.SetMat4("model", glm::mat4(1));
    AssetManager::BindMaterialByIndex(AssetManager::GetMaterialIndex("Ceiling"));
    //AssetManager::BindMaterialByIndex(AssetManager::GetMaterialIndex("NumGrid"));
    for (Wall& wall : Scene::_walls) {
     //   AssetManager::BindMaterialByIndex(wall.materialIndex);
        wall.Draw();
    }

    for (Floor& floor : Scene::_floors) {
        AssetManager::BindMaterialByIndex(floor.materialIndex);
        //  AssetManager::BindMaterialByIndex(AssetManager::GetMaterialIndex("NumGrid"));
        floor.Draw();
    }

    AssetManager::BindMaterialByIndex(AssetManager::GetMaterialIndex("Ceiling"));
    for (Ceiling& ceiling : Scene::_ceilings) {
        //AssetManager::BindMaterialByIndex(ceiling.materialIndex);
        ceiling.Draw();
    }

    static int trimCeilingMaterialIndex = AssetManager::GetMaterialIndex("Trims");
    static int trimFloorMaterialIndex = AssetManager::GetMaterialIndex("Door");

    // Ceiling trims
   AssetManager::BindMaterialByIndex(AssetManager::GetMaterialIndex("Ceiling"));
    for (Wall& wall : Scene::_walls) {
        for (auto& transform : wall.ceilingTrims) {
            shader.SetMat4("model", transform.to_mat4());
            AssetManager::GetModel("TrimCeiling")->Draw();
        }
    } 
    // Floor trims
    AssetManager::BindMaterialByIndex(trimFloorMaterialIndex);
    for (Wall& wall : Scene::_walls) {
        for (auto& transform : wall.floorTrims) {
            shader.SetMat4("model", transform.to_mat4());
            AssetManager::GetModel("TrimFloor")->Draw();
        }
    }

    // Render game objects
    for (GameObject& gameObject : Scene::_gameObjects) {
        shader.SetMat4("model", gameObject.GetModelMatrix());
        for (int i = 0; i < gameObject._meshMaterialIndices.size(); i++) {
            AssetManager::BindMaterialByIndex(gameObject._meshMaterialIndices[i]);
            gameObject._model->_meshes[i].Draw();
        }
    }

    for (Door& door : Scene::_doors) {

        AssetManager::BindMaterialByIndex(AssetManager::GetMaterialIndex("Door"));
        shader.SetMat4("model", door.GetFrameModelMatrix());
        auto* doorFrameModel = AssetManager::GetModel("DoorFrame");
        doorFrameModel->Draw();

        shader.SetMat4("model", door.GetDoorModelMatrix());
        auto* doorModel = AssetManager::GetModel("Door");
        doorModel->Draw();
    }



    // This is a hack. Fix this.
    AssetManager::BindMaterialByIndex(AssetManager::GetMaterialIndex("Glock"));
    for (auto& matrix : _glockMatrices) {
        shader.SetMat4("model", matrix);
        AssetManager::GetModel("Glock_Isolated")->Draw();
    }
    for (auto& matrix : _aks74uMatrices) {
        shader.SetMat4("model", matrix);
        AssetManager::GetModel("AKS74U_Isolated")->Draw();
    }


    // Draw physics objects

    // remove everything below here soon
    PxScene* scene = Physics::GetScene();
    PxU32 nbActors = scene->getNbActors(PxActorTypeFlag::eRIGID_DYNAMIC | PxActorTypeFlag::eRIGID_STATIC);
    if (nbActors)
    {
        std::vector<PxRigidActor*> actors(nbActors);
        scene->getActors(PxActorTypeFlag::eRIGID_DYNAMIC | PxActorTypeFlag::eRIGID_STATIC, reinterpret_cast<PxActor**>(&actors[0]), nbActors);
        
        for (PxRigidActor* actor : actors) {

            if (!actor) {
                std::cout << "you have an null ptr actor mate, and " << nbActors << " total actors\n";
                continue;
            }

            const PxU32 nbShapes = actor->getNbShapes();
            PxShape* shapes[32];
            actor->getShapes(shapes, nbShapes);

            for (PxU32 j = 0; j < nbShapes; j++) {
                const PxGeometry& geom = shapes[j]->getGeometry();

                auto& shape = shapes[j];
                if (shape->getQueryFilterData().word3 == PhysicsObjectType::CUBE ) {
                  //  shape->getQueryFilterData().word3 == PhysicsObjectType::CASING_PROJECTILE) {

                //if (geom.getType() == PxGeometryType::eBOX) {
              
                    //const PxMat44 shapePose(PxShapeExt::getGlobalPose(*shapes[j], *actor));
                    
                    const PxBoxGeometry& boxGeom = static_cast<const PxBoxGeometry&>(geom);
                    Transform localTransform;
                    localTransform.scale = glm::vec3(boxGeom.halfExtents.x, boxGeom.halfExtents.y, boxGeom.halfExtents.z);
                    localTransform.scale *= glm::vec3(2.0f);

                    glm::mat4 model = Util::PxMat44ToGlmMat4(actor->getGlobalPose()) * localTransform.to_mat4();
                    shader.SetMat4("model", model);
                    AssetManager::BindMaterialByIndex(AssetManager::GetMaterialIndex("Ceiling"));
                   // AssetManager::GetModel("Cube").Draw();
                }

             /*   if (shape->getQueryFilterData().word3 == PhysicsObjectType::CASING_PROJECTILE_GLOCK) {
                    Transform localTransform;
                    localTransform.scale *= glm::vec3(2.0f);
                    glm::mat4 model = Util::PxMat44ToGlmMat4(actor->getGlobalPose()) * localTransform.to_mat4();
                    shader.SetMat4("model", model);
                    AssetManager::BindMaterialByIndex(AssetManager::GetMaterialIndex("BulletCasing"));
                    AssetManager::GetModel("BulletCasing").Draw();
                }

                if (shape->getQueryFilterData().word3 == PhysicsObjectType::CASING_PROJECTILE_AKS74U) {
                    Transform localTransform;
                    localTransform.scale *= glm::vec3(2.0f);
                    glm::mat4 model = Util::PxMat44ToGlmMat4(actor->getGlobalPose()) * localTransform.to_mat4();
                    shader.SetMat4("model", model);
                    AssetManager::BindMaterialByIndex(AssetManager::GetMaterialIndex("Casing_AkS74U"));
                    AssetManager::GetModel("BulletCasing_AK").Draw();
                }*/
            }
        }
    }


/*
    Player& player = Scene::_players[0];
    const glm::vec3& origin = player.GetViewPos();
    const glm::vec3& direction = player.GetCameraForward() * glm::vec3(-1, -1, -1);;
    PhysXRayResult rayResult = Util::CastPhysXRay(origin, direction, 250);
       
    static int count = 0;

    // Check for any hits
    if (rayResult.hitFound) {
        count++;
        //std::cout << count << " you hit a cube bro\n";

        if (Input::LeftMousePressed()) {
            PxVec3 force = PxVec3(direction.x, direction.y, direction.z) * 100;
            PxRigidDynamic* actor = (PxRigidDynamic*)rayResult.hitActor;
            actor->addForce(force);
        }
    }*/
}

void DrawAnimatedObject(Shader& shader, AnimatedGameObject* animatedGameObject) {

    if (!animatedGameObject) {
        std::cout << "You tried to draw an nullptr AnimatedGameObject\n";
        return;
    }
    if (!animatedGameObject->_skinnedModel) {
        std::cout << "You tried to draw an AnimatedGameObject with a nullptr skinned model\n";
        return;
    }

    std::vector<glm::mat4> tempSkinningMats;
    for (unsigned int i = 0; i < animatedGameObject->_animatedTransforms.local.size(); i++) {
        glm::mat4 matrix = animatedGameObject->_animatedTransforms.local[i];
        shader.SetMat4("skinningMats[" + std::to_string(i) + "]", matrix);
        tempSkinningMats.push_back(matrix);
    }
    shader.SetMat4("model", animatedGameObject->GetModelMatrix());

  //  std::cout << "\n";

  //  if(Input::KeyPressed(HELL_KEY_T)) {
        if (animatedGameObject->GetName() == "Glock") {
            glm::vec3 barrelPosition = animatedGameObject->GetGlockCasingSpawnPostion();
            Point point = Point(barrelPosition, BLUE);
            Renderer::QueuePointForDrawing(point);
            //            debugPoints.push_back(barrelPosition);

        }

   // }
    
    for (int i = 0; i < animatedGameObject->_animatedTransforms.worldspace.size(); i++) {





        auto& bone = animatedGameObject->_animatedTransforms.worldspace[i];
        glm::mat4 m = animatedGameObject->GetModelMatrix() * bone;
        float x = m[3][0];
        float y = m[3][1];
        float z = m[3][2];
        Point p2(glm::vec3(x, y, z), RED);
        Renderer::QueuePointForDrawing(p2);



        if (animatedGameObject->_animatedTransforms.names[i] == "Glock") {
        //    float x = m[3][0];
        //    float y = m[3][1];
        //    float z = m[3][2];
         //   debugPoints.push_back(glm::vec3(x, y, z));
        }

       // std::cout << i << ": " << animatedGameObject->_animatedTransforms.names[i] << "\n";

     /*   if (animatedGameObject->_animatedTransforms.names[i] == "Glock") {
            _glockMatrices.push_back(m);
        }
        if (animatedGameObject->_animatedTransforms.names[i] == "Glock") {
            _aks74uMatrices.push_back(m);
        }
        */

        if (animatedGameObject->GetName() == "Shotgun") {
            glm::vec3 barrelPosition = animatedGameObject->GetShotgunBarrelPostion();
            Point point = Point(barrelPosition, BLUE);
            //   QueuePointForDrawing(point);
        }
    }

    static bool maleHands = true;
    if (Input::KeyPressed(HELL_KEY_U)) {
        maleHands = !maleHands;
    }

    glEnable(GL_CULL_FACE);

    SkinnedModel& skinnedModel = *animatedGameObject->_skinnedModel;
    glBindVertexArray(skinnedModel.m_VAO);
    for (int i = 0; i < skinnedModel.m_meshEntries.size(); i++) {
        AssetManager::BindMaterialByIndex(animatedGameObject->_materialIndices[i]);
        if (maleHands) {
            if (skinnedModel.m_meshEntries[i].Name == "SK_FPSArms_Female.001" ||
                skinnedModel.m_meshEntries[i].Name == "SK_FPSArms_Female") {
                continue;
            }
        }
        else {
            if (skinnedModel.m_meshEntries[i].Name == "manniquen1_2.001" ||
                skinnedModel.m_meshEntries[i].Name == "manniquen1_2") {
                continue;
            }
        }
        glDrawElementsBaseVertex(GL_TRIANGLES, skinnedModel.m_meshEntries[i].NumIndices, GL_UNSIGNED_INT, (void*)(sizeof(unsigned int) * skinnedModel.m_meshEntries[i].BaseIndex), skinnedModel.m_meshEntries[i].BaseVertex);
    }
}

void DrawAnimatedScene(Shader& shader, Player* player) {

    // This is a temporary hack so multiple animated game objects can have glocks which are queued to be rendered later by DrawScene()
    _glockMatrices.clear();
    _aks74uMatrices.clear();

    for (Player& otherPlayer : Scene::_players) {
        if (&otherPlayer != player) {
            for (int i = 0; i < otherPlayer._characterModel._animatedTransforms.worldspace.size(); i++) {
                auto& bone = otherPlayer._characterModel._animatedTransforms.worldspace[i];
                glm::mat4 m = otherPlayer._characterModel.GetModelMatrix() * bone;
                if (otherPlayer._characterModel._animatedTransforms.names[i] == "Glock") {
                    if (otherPlayer.GetCurrentWeaponIndex() == GLOCK) {
                        _glockMatrices.push_back(m);
                    }
                    if (otherPlayer.GetCurrentWeaponIndex() == AKS74U) {
                        _aks74uMatrices.push_back(m);
                    }
                }
            }
        }
    }

    shader.Use();
    shader.SetBool("isAnimated", true);

    shader.SetMat4("model", glm::mat4(1)); // 1.0 for weapon, 0.9 for scene.

    // Render other players
    for (Player& otherPlayer : Scene::_players) {
        if (&otherPlayer != player) {
            DrawAnimatedObject(shader, &otherPlayer._characterModel);            
        }
    }


    shader.SetMat4("model", glm::mat4(1)); // 1.0 for weapon, 0.9 for scene.
    shader.SetFloat("projectionMatrixIndex", 0.0f);
    for (auto& animatedObject : Scene::GetAnimatedGameObjects()) {
        DrawAnimatedObject(shader, &animatedObject);
    }

    glDisable(GL_CULL_FACE);
    shader.SetFloat("projectionMatrixIndex", 1.0f);
    shader.SetMat4("projection", Renderer::GetProjectionMatrix(_depthOfFieldWeapon)); // 1.0 for weapon, 0.9 for scene.
    DrawAnimatedObject(shader, &player->GetFirstPersonWeapon());
    shader.SetFloat("projectionMatrixIndex", 0.0f);
    glEnable(GL_CULL_FACE);

    shader.SetBool("isAnimated", false);
}

void DrawShadowMapScene(Shader& shader) {

    shader.SetMat4("model", glm::mat4(1));
    for (Wall& wall : Scene::_walls) {
        wall.Draw();
    }
    for (Floor& floor : Scene::_floors) {
        floor.Draw();
    }
    for (Ceiling& ceiling : Scene::_ceilings) {
        ceiling.Draw();
    }
    for (GameObject& gameObject : Scene::_gameObjects) {
        shader.SetMat4("model", gameObject.GetModelMatrix());
        for (int i = 0; i < gameObject._meshMaterialIndices.size(); i++) {
            gameObject._model->_meshes[i].Draw();
        }
    }

    for (Door& door : Scene::_doors) {
        shader.SetMat4("model", door.GetFrameModelMatrix());
        auto* doorFrameModel = AssetManager::GetModel("DoorFrame");
        doorFrameModel->Draw();
        shader.SetMat4("model", door.GetDoorModelMatrix());
        auto* doorModel = AssetManager::GetModel("Door");
        doorModel->Draw();
    }
}

void RenderImmediate() {
    glBindVertexArray(_pointLineVAO);
    glBindBuffer(GL_ARRAY_BUFFER, _pointLineVBO);
    // Draw triangles
    glBufferData(GL_ARRAY_BUFFER, _solidTrianglePoints.size() * sizeof(Point), _solidTrianglePoints.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Point), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Point), (void*)offsetof(Point, color));
    glBindVertexArray(_pointLineVAO);
    glBindVertexArray(_pointLineVAO);
    glDrawArrays(GL_TRIANGLES, 0, _solidTrianglePoints.size());
    // Draw lines
    glBufferData(GL_ARRAY_BUFFER, _lines.size() * sizeof(Line), _lines.data(), GL_STATIC_DRAW);
    glDrawArrays(GL_LINES, 0, 2 * _lines.size());
    // Draw points
    glBufferData(GL_ARRAY_BUFFER, _points.size() * sizeof(Point), _points.data(), GL_STATIC_DRAW);
    glBindVertexArray(_pointLineVAO);
    glDrawArrays(GL_POINTS, 0, _points.size());
    // Cleanup
    _lines.clear();
    _points.clear();
    _solidTrianglePoints.clear();
}

void Renderer::HotloadShaders() {

    std::cout << "Hotloaded shaders\n";

    _shaders.solidColor.Load("solid_color.vert", "solid_color.frag");
    _shaders.UI.Load("ui.vert", "ui.frag");
    _shaders.editorSolidColor.Load("editor_solid_color.vert", "editor_solid_color.frag");
    _shaders.composite.Load("composite.vert", "composite.frag");
    _shaders.fxaa.Load("fxaa.vert", "fxaa.frag");
    _shaders.animatedQuad.Load("animated_quad.vert", "animated_quad.frag");
    _shaders.depthOfField.Load("depth_of_field.vert", "depth_of_field.frag");
    _shaders.debugViewPointCloud.Load("debug_view_point_cloud.vert", "debug_view_point_cloud.frag");
    _shaders.geometry.Load("geometry.vert", "geometry.frag");
    _shaders.lighting.Load("lighting.vert", "lighting.frag");
    _shaders.debugViewPropgationGrid.Load("debug_view_propogation_grid.vert", "debug_view_propogation_grid.frag");
    _shaders.editorTextured.Load("editor_textured.vert", "editor_textured.frag");
    _shaders.bulletDecals.Load("bullet_decals.vert", "bullet_decals.frag");
    _shaders.geometry_instanced.Load("geometry_instanced.vert", "geometry_instanced.frag");

    _shaders.compute.Load("res/shaders/compute.comp");
    _shaders.pointCloud.Load("res/shaders/point_cloud.comp");
    _shaders.propogateLight.Load("res/shaders/propogate_light.comp");
    //_shaders.propogationList.Load("res/shaders/propogation_list.comp");
    //_shaders.calculateIndirectDispatchSize.Load("res/shaders/calculate_inidrect_dispatch_size.comp");
}

void Renderer::RenderEditorFrame() {

    PlayerRenderTarget playerRenderTarget = GetPlayerRenderTarget(0);
    PresentFrameBuffer presentFrameBuffer = playerRenderTarget.presentFrameBuffer;

    presentFrameBuffer.Bind();

    float renderWidth = (float)presentFrameBuffer.GetWidth();
    float renderHeight = (float)presentFrameBuffer.GetHeight();
    //float screenWidth = GL::GetWindowWidth();
    //float screenHeight = GL::GetWindowHeight();

    glViewport(0, 0, renderWidth, renderHeight);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glEnable(GL_DEPTH_TEST);

    _shaders.editorTextured.Use();
    _shaders.editorTextured.SetMat4("projection", Editor::GetProjectionMatrix());
    _shaders.editorTextured.SetMat4("view", Editor::GetViewMatrix());
    Transform t;
    t.position.y = -3.5f;
    _shaders.editorTextured.SetMat4("model", t.to_mat4());

    for (Floor& floor : Scene::_floors) {
        AssetManager::BindMaterialByIndex(floor.materialIndex);
        floor.Draw();
    }

    _shaders.editorSolidColor.Use();
    _shaders.editorSolidColor.SetMat4("projection", Editor::GetProjectionMatrix());
    _shaders.editorSolidColor.SetMat4("view", Editor::GetViewMatrix());
    _shaders.editorSolidColor.SetBool("uniformColor", false);

    RenderImmediate();

   
    // Render UI
    glBindFramebuffer(GL_FRAMEBUFFER, presentFrameBuffer.GetID());
    glViewport(0, 0, presentFrameBuffer.GetWidth(), presentFrameBuffer.GetHeight());
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    Renderer::RenderUI();

    // Blit image back to frame buffer
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, presentFrameBuffer.GetID());
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, presentFrameBuffer.GetWidth(), presentFrameBuffer.GetHeight(), 0, 0, GL::GetWindowWidth(), GL::GetWindowHeight(), GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

void Renderer::WipeShadowMaps() {

    for (ShadowMap& shadowMap : _shadowMaps) {
        shadowMap.Clear();
    }
}

void Renderer::ToggleDrawingLights() {
    _toggles.drawLights = !_toggles.drawLights;
}
void Renderer::ToggleDrawingProbes() {
    _toggles.drawProbes = !_toggles.drawProbes;
}
void Renderer::ToggleDrawingLines() {
    _toggles.drawLines = !_toggles.drawLines;
}
void Renderer::ToggleCollisionWorld() {
    _toggles.drawCollisionWorld = !_toggles.drawCollisionWorld;
}


static Mesh _quadMesh;

inline void DrawQuad(int viewportWidth, int viewPortHeight, int textureWidth, int textureHeight, int xPosition, int yPosition, bool centered = false, float scale = 1.0f, int xSize = -1, int ySize = -1 /*, int xClipMin = -1, int xClipMax = -1, int yClipMin = -1, int yClipMax = -1*/) {

    float quadWidth = (float)xSize;
    float quadHeight = (float)ySize;
    if (xSize == -1) {
        quadWidth = (float)textureWidth;
    }
    if (ySize == -1) {
        quadHeight = (float)textureHeight;
    }
    if (centered) {
        xPosition -= (int)(quadWidth / 2);
        yPosition -= (int)(quadHeight / 2);
    }
    float renderTargetWidth = (float)viewportWidth;
    float renderTargetHeight = (float)viewPortHeight;
    float width = (1.0f / renderTargetWidth) * quadWidth * scale;
    float height = (1.0f / renderTargetHeight) * quadHeight * scale;
    float ndcX = ((xPosition + (quadWidth / 2.0f)) / renderTargetWidth) * 2 - 1;
    float ndcY = ((yPosition + (quadHeight / 2.0f)) / renderTargetHeight) * 2 - 1;
    Transform transform;
    transform.position.x = ndcX;
    transform.position.y = ndcY * -1;
    transform.scale = glm::vec3(width, height * -1, 1);
    _shaders.UI.SetMat4("model", transform.to_mat4());
    _quadMesh.Draw();
}

void Renderer::QueueUIForRendering(std::string textureName, int screenX, int screenY, bool centered) {
    UIRenderInfo info;
    info.textureName = textureName;
    info.screenX = screenX;
    info.screenY = screenY;
    info.centered = centered;
    _UIRenderInfos.push_back(info);
}
void Renderer::QueueUIForRendering(UIRenderInfo renderInfo) {
    _UIRenderInfos.push_back(renderInfo);
}

void Renderer::RenderUI() {

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquation(GL_FUNC_ADD);
    _shaders.UI.Use();

    if (_quadMesh.GetIndexCount() == 0) {
        Vertex vertA, vertB, vertC, vertD;
        vertA.position = { -1.0f, -1.0f, 0.0f };
        vertB.position = { -1.0f, 1.0f, 0.0f };
        vertC.position = { 1.0f,  1.0f, 0.0f };
        vertD.position = { 1.0f,  -1.0f, 0.0f };
        vertA.uv = { 0.0f, 0.0f };
        vertB.uv = { 0.0f, 1.0f };
        vertC.uv = { 1.0f, 1.0f };
        vertD.uv = { 1.0f, 0.0f };
        std::vector<Vertex> vertices;
        vertices.push_back(vertA);
        vertices.push_back(vertB);
        vertices.push_back(vertC);
        vertices.push_back(vertD);
        std::vector<uint32_t> indices = { 0, 1, 2, 0, 2, 3 };
        _quadMesh = Mesh(vertices, indices, "QuadMesh");
    }

    float viewportWidth = (float)_renderWidth;
    float viewportHeight = (float)_renderHeight;
    if (_viewportMode == SPLITSCREEN) {
        viewportHeight *= 0.5f;
    }

    for (UIRenderInfo& uiRenderInfo : _UIRenderInfos) {
        AssetManager::GetTexture(uiRenderInfo.textureName)->Bind(0);
        Texture* texture = AssetManager::GetTexture(uiRenderInfo.textureName);
        DrawQuad(viewportWidth, viewportHeight, texture->GetWidth(), texture->GetHeight(), uiRenderInfo.screenX, uiRenderInfo.screenY, uiRenderInfo.centered);
    }

    _UIRenderInfos.clear();
    glDisable(GL_BLEND);
}

int Renderer::GetRenderWidth() {
    return _renderWidth;
}

int Renderer::GetRenderHeight() {
    return _renderHeight;
}

float Renderer::GetPointCloudSpacing() {
    return _pointCloudSpacing;
}

void Renderer::NextMode() {
    _mode = (RenderMode)(int(_mode) + 1);
    if (_mode == MODE_COUNT)
        _mode = (RenderMode)0;
}

void Renderer::PreviousMode() {
    if (int(_mode) == 0)
        _mode = RenderMode(int(MODE_COUNT) - 1);
    else
        _mode = (RenderMode)(int(_mode) - 1);
}

void Renderer::NextDebugLineRenderMode() {
    _debugLineRenderMode = (DebugLineRenderMode)(int(_debugLineRenderMode) + 1);
    if (_debugLineRenderMode == DEBUG_LINE_MODE_COUNT)
        _debugLineRenderMode = (DebugLineRenderMode)0;
}

glm::mat4 Renderer::GetProjectionMatrix(float depthOfField) {

    float width = (float)GL::GetWindowWidth();
    float height = (float)GL::GetWindowHeight();

    if (_viewportMode == SPLITSCREEN) {
        height *= 0.5f;
    }

    return glm::perspective(depthOfField, width / height, NEAR_PLANE, FAR_PLANE);
}

void Renderer::QueueLineForDrawing(Line line) {
    _lines.push_back(line);
}

void Renderer::QueuePointForDrawing(Point point) {
    _points.push_back(point);
}

void Renderer::QueueTriangleForLineRendering(Triangle& triangle) {
    _lines.push_back(Line(triangle.p1, triangle.p2, triangle.color));
    _lines.push_back(Line(triangle.p2, triangle.p3, triangle.color));
    _lines.push_back(Line(triangle.p3, triangle.p1, triangle.color));
}

void Renderer::QueueTriangleForSolidRendering(Triangle& triangle) {
    _solidTrianglePoints.push_back(Point(triangle.p1, triangle.color));
    _solidTrianglePoints.push_back(Point(triangle.p2, triangle.color));
    _solidTrianglePoints.push_back(Point(triangle.p3, triangle.color));
}

void DrawFullscreenQuad() {
    static GLuint vao = 0;
    if (vao == 0) {
        float vertices[] = {
            // positions         texcoords
            -1.0f,  1.0f, 0.0f,  0.0f, 1.0f,
            -1.0f, -1.0f, 0.0f,  0.0f, 0.0f,
             1.0f,  1.0f, 0.0f,  1.0f, 1.0f,
             1.0f, -1.0f, 0.0f,  1.0f, 0.0f,
        };
        unsigned int vbo;
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), &vertices, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
    }
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

void DrawMuzzleFlashes(Player* player) {

    int playerIndex = GetPlayerIndexFromPlayerPointer(player);
    PlayerRenderTarget& playerRenderTarget = GetPlayerRenderTarget(playerIndex);
    GBuffer& gBuffer = playerRenderTarget.gBuffer;
    //PresentFrameBuffer& presentFrameBuffer = playerRenderTarget.presentFrameBuffer;

    static MuzzleFlash muzzleFlash; // has init on the first use. DISGUSTING. Fix if you ever see this when you aren't on another mission.

    // Bail if no flash    
    if (player->GetMuzzleFlashTime() < 0)
        return;
    if (player->GetMuzzleFlashTime() > 1)
        return;

    gBuffer.Bind();
    glDrawBuffer(GL_COLOR_ATTACHMENT3);
    glViewport(0, 0, gBuffer.GetWidth(), gBuffer.GetHeight());

    muzzleFlash.m_CurrentTime = player->GetMuzzleFlashTime();
    glm::vec3 worldPosition = glm::vec3(0);
    if (player->GetCurrentWeaponIndex() == GLOCK) {
        worldPosition = player->GetFirstPersonWeapon().GetGlockBarrelPostion();
    }
    else if (player->GetCurrentWeaponIndex() == AKS74U) {
        worldPosition = player->GetFirstPersonWeapon().GetAKS74UBarrelPostion();
    }
    else if (player->GetCurrentWeaponIndex() == SHOTGUN) {
        worldPosition = player->GetFirstPersonWeapon().GetShotgunBarrelPostion();
    }
    else {
        return;
    }

    Transform t;
    t.position = worldPosition;
    t.rotation = player->GetViewRotation();

    // draw to lighting shader
    glDrawBuffer( GL_COLOR_ATTACHMENT3);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /// this is sketchy. add this to player class.
    glm::mat4 projection = Renderer::GetProjectionMatrix(_depthOfFieldWeapon); // 1.0 for weapon, 0.9 for scene.
    glm::mat4 view = player->GetViewMatrix();
    glm::vec3 viewPos = player->GetViewPos();

    _shaders.animatedQuad.Use();
    _shaders.animatedQuad.SetMat4("u_MatrixProjection", projection);
    _shaders.animatedQuad.SetMat4("u_MatrixView", view);
    _shaders.animatedQuad.SetVec3("u_ViewPos", viewPos);

    glActiveTexture(GL_TEXTURE0);
    AssetManager::GetTexture("MuzzleFlash_ALB")->Bind(0);

    muzzleFlash.Draw(&_shaders.animatedQuad, t, player->GetMuzzleFlashRotation());
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
}

void DrawInstanced(Mesh& mesh, std::vector<glm::mat4>& matrices) {
    if (_ssbos.instanceMatrices == 0) {
        glCreateBuffers(1, &_ssbos.instanceMatrices);
        glNamedBufferStorage(_ssbos.instanceMatrices, 4096 * sizeof(glm::mat4), NULL, GL_DYNAMIC_STORAGE_BIT);
    }
    if (matrices.size()) {
        glNamedBufferSubData(_ssbos.instanceMatrices, 0, matrices.size() * sizeof(glm::mat4), &matrices[0]);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, _ssbos.instanceMatrices);
        glBindVertexArray(mesh._VAO);
        glDrawElementsInstanced(GL_TRIANGLES, mesh._indexCount, GL_UNSIGNED_INT, 0, matrices.size());
    }
}

void DrawBulletDecals(Player* player) {

    std::vector<glm::mat4> matrices;
    for (Decal& decal : Scene::_decals) {
        matrices.push_back(decal.GetModelMatrix());
    }

    glm::mat4 projection = Renderer::GetProjectionMatrix(_depthOfFieldScene); // 1.0 for weapon, 0.9 for scene.
    glm::mat4 view = player->GetViewMatrix();

    _shaders.bulletDecals.Use();
    _shaders.bulletDecals.SetMat4("projection", projection);
    _shaders.bulletDecals.SetMat4("view", view);

    AssetManager::BindMaterialByIndex(AssetManager::GetMaterialIndex("BulletHole_Plaster"));

    DrawInstanced(AssetManager::GetDecalMesh(), matrices);
}


void DrawCasingProjectiles(Player* player) {

    glm::mat4 projection = Renderer::GetProjectionMatrix(_depthOfFieldScene); // 1.0 for weapon, 0.9 for scene.
    glm::mat4 view = player->GetViewMatrix();
    _shaders.geometry_instanced.Use();
    _shaders.geometry_instanced.SetMat4("projection", projection);
    _shaders.geometry_instanced.SetMat4("view", view);

    // GLOCK
    std::vector<glm::mat4> matrices;
    for (BulletCasing& casing : Scene::_bulletCasings) {
        if (casing.type == GLOCK) {
            matrices.push_back(casing.GetModelMatrix());
        }
    }
    AssetManager::BindMaterialByIndex(AssetManager::GetMaterialIndex("BulletCasing"));
    Mesh& glockCasingmesh = AssetManager::GetModel("BulletCasing")->_meshes[0];
    DrawInstanced(glockCasingmesh, matrices);

    // AKS74U
    matrices.clear();
    for (BulletCasing& casing : Scene::_bulletCasings) {
        if (casing.type == AKS74U) {
            matrices.push_back(casing.GetModelMatrix());
        }
    }
    AssetManager::BindMaterialByIndex(AssetManager::GetMaterialIndex("Casing_AkS74U"));
    Mesh& aks75uCasingmesh = AssetManager::GetModel("BulletCasing_AK")->_meshes[0];
    DrawInstanced(aks75uCasingmesh, matrices);
}

void RenderShadowMaps() {

    if (!Renderer::_shadowMapsAreDirty)
        return;

    _shaders.shadowMap.Use();
    _shaders.shadowMap.SetFloat("far_plane", SHADOW_FAR_PLANE);
    _shaders.shadowMap.SetMat4("model", glm::mat4(1));
    glDepthMask(true);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);

    // clear all, incase there are less lights now
    glEnable(GL_DEPTH_TEST);
    glViewport(0, 0, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);
    for (int i = 0; i < _shadowMaps.size(); i++) {
        glBindFramebuffer(GL_FRAMEBUFFER, _shadowMaps[i]._ID);
        glClear(GL_DEPTH_BUFFER_BIT);
    }

    for (int i = 0; i < Scene::_lights.size(); i++) {
        glBindFramebuffer(GL_FRAMEBUFFER, _shadowMaps[i]._ID);
        std::vector<glm::mat4> projectionTransforms;
        glm::vec3 position = Scene::_lights[i].position;
        glm::mat4 shadowProj = glm::perspective(glm::radians(90.0f), (float)SHADOW_MAP_SIZE / (float)SHADOW_MAP_SIZE, SHADOW_NEAR_PLANE, SHADOW_FAR_PLANE);
        projectionTransforms.clear();
        projectionTransforms.push_back(shadowProj * glm::lookAt(position, position + glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)));
        projectionTransforms.push_back(shadowProj * glm::lookAt(position, position + glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)));
        projectionTransforms.push_back(shadowProj * glm::lookAt(position, position + glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f)));
        projectionTransforms.push_back(shadowProj * glm::lookAt(position, position + glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f)));
        projectionTransforms.push_back(shadowProj * glm::lookAt(position, position + glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, -1.0f, 0.0f)));
        projectionTransforms.push_back(shadowProj * glm::lookAt(position, position + glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, -1.0f, 0.0f)));
        _shaders.shadowMap.SetMat4("shadowMatrices[0]", projectionTransforms[0]);
        _shaders.shadowMap.SetMat4("shadowMatrices[1]", projectionTransforms[1]);
        _shaders.shadowMap.SetMat4("shadowMatrices[2]", projectionTransforms[2]);
        _shaders.shadowMap.SetMat4("shadowMatrices[3]", projectionTransforms[3]);
        _shaders.shadowMap.SetMat4("shadowMatrices[4]", projectionTransforms[4]);
        _shaders.shadowMap.SetMat4("shadowMatrices[5]", projectionTransforms[5]);
        _shaders.shadowMap.SetVec3("lightPosition", position);
        _shaders.shadowMap.SetMat4("model", glm::mat4(1));
        DrawShadowMapScene(_shaders.shadowMap);
    }
    //Renderer::_shadowMapsAreDirty = false;
}

void Renderer::CreatePointCloudBuffer() {

    if (Scene::_cloudPoints.empty()) {
        return;
    }

    _pointCloud.vertexCount = Scene::_cloudPoints.size();
    if (_pointCloud.VAO != 0) {
        glDeleteBuffers(1, &_pointCloud.VAO);
        glDeleteVertexArrays(1, &_pointCloud.VAO);
    }
    glGenVertexArrays(1, &_pointCloud.VAO);
    glGenBuffers(1, &_pointCloud.VBO);
    glBindVertexArray(_pointCloud.VAO);
    glBindBuffer(GL_ARRAY_BUFFER, _pointCloud.VBO);
    glBufferData(GL_ARRAY_BUFFER, _pointCloud.vertexCount * sizeof(CloudPoint), &Scene::_cloudPoints[0], GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(CloudPoint), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(CloudPoint), (void*)offsetof(CloudPoint, normal));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(CloudPoint), (void*)offsetof(CloudPoint, directLighting));
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

    // Floor vertices
   /* std::vector<glm::vec4> vertices;
    for (Floor& floor : Scene::_floors) {
        for (Vertex& vertex : floor.vertices) {
            vertices.push_back(glm::vec4(vertex.position.x, vertex.position.y, vertex.position.z, 0));
        }
    }
    if (_ssbos.floorVertices != 0) {
        glDeleteBuffers(1, &_ssbos.floorVertices);
    }
    glGenBuffers(1, &_ssbos.floorVertices);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, _ssbos.floorVertices);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER, vertices.size() * sizeof(glm::vec4), &vertices[0], GL_DYNAMIC_STORAGE_BIT);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);  
    _floorVertexCount = vertices.size();
    std::cout << "You sent " << _floorVertexCount << " to the GPU\n";*/
}

void DrawPointCloud(Player* player) {
    _shaders.debugViewPointCloud.Use();
    _shaders.debugViewPointCloud.SetMat4("projection", Renderer::GetProjectionMatrix(_depthOfFieldScene));
    _shaders.debugViewPointCloud.SetMat4("view", player->GetViewMatrix());
    //glBindBuffer(GL_SHADER_STORAGE_BUFFER, _ssbos.pointCloud);
    //glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, _ssbos.pointCloud);
    glDisable(GL_DEPTH_TEST);
    glBindVertexArray(_pointCloud.VAO);
    glDrawArrays(GL_POINTS, 0, _pointCloud.vertexCount);
    glBindVertexArray(0);
}

void InitCompute() {
    glGenTextures(1, &_progogationGridTexture);
    glBindTexture(GL_TEXTURE_3D, _progogationGridTexture);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA16F, _gridTextureWidth, _gridTextureHeight, _gridTextureDepth, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindImageTexture(1, _progogationGridTexture, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA16F);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_3D, _progogationGridTexture);

    // Create ssbos
    //glGenBuffers(1, &_ssbos.indirectDispatchSize);
    //glGenBuffers(1, &_ssbos.atomicCounter);
    //glDeleteBuffers(1, &_ssbos.propogationList);
    glDeleteBuffers(1, &_ssbos.rtVertices);
    glDeleteBuffers(1, &_ssbos.rtMesh);
    glGenBuffers(1, &_ssbos.rtInstances);
    glGenBuffers(1, &_ssbos.dirtyPointCloudIndices);

    _shaders.compute.Load("res/shaders/compute.comp");
    _shaders.pointCloud.Load("res/shaders/point_cloud.comp");
    _shaders.propogateLight.Load("res/shaders/propogate_light.comp");
    //_shaders.propogationList.Load("res/shaders/propogation_list.comp");
   // _shaders.calculateIndirectDispatchSize.Load("res/shaders/calculate_inidrect_dispatch_size.comp");

    
    Scene::CreatePointCloud();
    Renderer::CreatePointCloudBuffer();
    Renderer::CreateTriangleWorldVertexBuffer();
    
    std::cout << "Point cloud has " << Scene::_cloudPoints.size() << " points\n";
    std::cout << "Propogation grid has " << (_mapWidth * _mapHeight * _mapDepth / _propogationGridSpacing) << " cells\n";

    // Propogation List
   /*glGenBuffers(1, &_ssbos.propogationList);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, _ssbos.propogationList);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER, _gridTextureSize * sizeof(glm::uvec4), nullptr, GL_DYNAMIC_STORAGE_BIT);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    std::cout << "The propogation list has room for " << _gridTextureSize << " uvec4 elements\n";*/
}

void Renderer::CreateTriangleWorldVertexBuffer() {

    // Vertices
    std::vector<glm::vec4> vertices;
    for (glm::vec3 vertex : Scene::_rtVertices) {
        vertices.push_back(glm::vec4(vertex, 0.0f));
    }
    glGenBuffers(1, &_ssbos.rtVertices);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, _ssbos.rtVertices);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER, vertices.size() * sizeof(glm::vec4), &vertices[0], GL_DYNAMIC_STORAGE_BIT);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

    // Mesh
    glGenBuffers(1, &_ssbos.rtMesh);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, _ssbos.rtMesh);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER, Scene::_rtMesh.size() * sizeof(RTMesh), &Scene::_rtMesh[0], GL_DYNAMIC_STORAGE_BIT);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

    std::cout << "You are raytracing against " << (vertices.size() / 3) << " tris\n";
    std::cout << "You are raytracing " << Scene::_rtMesh.size() << " mesh\n";
}

void ComputePass() {

    // Update RT Instances
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, _ssbos.rtInstances);
    glBufferData(GL_SHADER_STORAGE_BUFFER, Scene::_rtInstances.size() * sizeof(RTInstance), &Scene::_rtInstances[0], GL_DYNAMIC_COPY);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

    UpdatePointCloudLighting();
    UpdatePropogationgGrid();
}



void UpdatePointCloudLighting() {
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, _ssbos.rtVertices);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, _pointCloud.VBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, _ssbos.rtMesh);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, _ssbos.rtInstances);
    _shaders.pointCloud.Use();
    _shaders.pointCloud.SetInt("meshCount", Scene::_rtMesh.size());
    _shaders.pointCloud.SetInt("instanceCount", Scene::_rtInstances.size());
    _shaders.pointCloud.SetInt("lightCount", std::min((int)Scene::_lights.size(), 32));

    auto& lights = Scene::_lights;
    for (int i = 0; i < lights.size(); i++) {
        _shaders.pointCloud.SetVec3("lights[" + std::to_string(i) + "].position", lights[i].position);
        _shaders.pointCloud.SetVec3("lights[" + std::to_string(i) + "].color", lights[i].color);
        _shaders.pointCloud.SetFloat("lights[" + std::to_string(i) + "].radius", lights[i].radius);
        _shaders.pointCloud.SetFloat("lights[" + std::to_string(i) + "].strength", lights[i].strength);
    }
 


    if (!_dirtyPointCloudIndices.empty()) {

        // Cloud point indices buffer
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, _ssbos.dirtyPointCloudIndices);
        glBufferData(GL_SHADER_STORAGE_BUFFER, _dirtyPointCloudIndices.size() * sizeof(int), &_dirtyPointCloudIndices[0], GL_STATIC_COPY);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, _ssbos.dirtyPointCloudIndices);

        //std::cout << "_cloudPointIndices.size(): " << _cloudPointIndices.size() << "\n";
        glDispatchCompute(std::ceil(_dirtyPointCloudIndices.size() / 64.0f), 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    }
}

void FindProbeCoordsWithinMapBounds() {

    _probeCoordsWithinMapBounds.clear();
    _probeWorldPositionsWithinMapBounds.clear();

    Timer t("FindProbeCoordsWithinMapBounds()");

    // Build a list of floor/ceiling vertices
    std::vector<glm::vec4> floorVertices;
    std::vector<glm::vec4> ceilingVertices;
    for (Floor& floor : Scene::_floors) {
        for (Vertex& vertex : floor.vertices) {
            floorVertices.emplace_back(vertex.position.x, vertex.position.y, vertex.position.z, floor.height);
        }
    }
    for (Ceiling& ceiling : Scene::_ceilings) {
        for (Vertex& vertex : ceiling.vertices) {
            ceilingVertices.emplace_back(vertex.position.x, vertex.position.y, vertex.position.z, ceiling.height);
        }
    }

    // Check which probes are within those bounds created above
    for (int z = 0; z < _gridTextureWidth; z++) {
        for (int y = 0; y < _gridTextureHeight; y++) {
            for (int x = 0; x < _gridTextureDepth; x++) {

             
                bool foundOne = false;

                glm::vec3 probePosition = glm::vec3(x, y, z) * _propogationGridSpacing;

                // Check floors
                for (int j = 0; j < floorVertices.size(); j += 3) {
                    if (probePosition.y < floorVertices[j].w) {
                        continue;
                    }
                    glm::vec2 probePos = glm::vec2(probePosition.x, probePosition.z);
                    glm::vec2 v1 = glm::vec2(floorVertices[j + 0].x, floorVertices[j + 0].z); // when you remove this gen code then it doesnt generate any gridIndices meaning no indirectLight
                    glm::vec2 v2 = glm::vec2(floorVertices[j + 1].x, floorVertices[j + 1].z);
                    glm::vec2 v3 = glm::vec2(floorVertices[j + 2].x, floorVertices[j + 2].z);

                    // If you are above one, check if you are also below a ceiling
                    if (Util::PointIn2DTriangle(probePos, v1, v2, v3)) {

                        if (probePosition.y < 2.6f) {
                            _probeCoordsWithinMapBounds.emplace_back(x, y, z, 0);
                            _probeWorldPositionsWithinMapBounds.emplace_back(probePosition);
                        }

                        /*for (int j = 0; j < ceilingVertices.size(); j += 3) {
                            if (probePosition.y > ceilingVertices[j].w) {
                                continue;
                            }
                            glm::vec2 v1c = glm::vec2(ceilingVertices[j + 0].x, ceilingVertices[j + 0].z);
                            glm::vec2 v2c = glm::vec2(ceilingVertices[j + 1].x, ceilingVertices[j + 1].z);
                            glm::vec2 v3c = glm::vec2(ceilingVertices[j + 2].x, ceilingVertices[j + 2].z);
                            if (Util::PointIn2DTriangle(probePos, v1c, v2c, v3c)) {
                                
                                if (!foundOne) {
                                    _probeCoordsWithinMapBounds.emplace_back(x, y, z, 0);
                                    foundOne = true;
                                }
                                //goto hell;
                            }
                        }*/
                    }
                }
            //hell: {}
            }
        }
    }

    std::cout << "There are " << _probeCoordsWithinMapBounds.size() << " probes within rooms\n";    
}

void UpdatePropogationgGrid() {
    
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, _ssbos.dirtyPointCloudIndices);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, _ssbos.rtVertices);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, _pointCloud.VBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, _ssbos.rtMesh);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, _ssbos.rtInstances);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_3D, _progogationGridTexture);
    _shaders.propogateLight.Use();
    _shaders.propogateLight.SetInt("pointCloudSize", Scene::_cloudPoints.size());
    _shaders.propogateLight.SetInt("meshCount", Scene::_rtMesh.size());
    _shaders.propogateLight.SetInt("instanceCount", Scene::_rtInstances.size());
    _shaders.propogateLight.SetFloat("propogationGridSpacing", _propogationGridSpacing);
    _shaders.propogateLight.SetFloat("maxDistance", _maxPropogationDistance);
    _shaders.propogateLight.SetInt("dirtyPointCloudIndexCount", _dirtyPointCloudIndices.size());

    if (_ssbos.dirtyGridCoordinates == 0) {
        glGenBuffers(1, &_ssbos.dirtyGridCoordinates);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, _ssbos.dirtyGridCoordinates);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glBufferData(GL_SHADER_STORAGE_BUFFER, _dirtyProbeCount * sizeof(glm::uvec4), &_dirtyProbeCoords[0], GL_DYNAMIC_COPY);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, _ssbos.dirtyGridCoordinates);

    if (_dirtyProbeCount > 0) {
        int invocationCount = (int)(std::ceil(_dirtyProbeCoords.size() / 64.0f));
        glDispatchCompute(invocationCount, 1, 1);
    }
}

void CalculateDirtyCloudPoints() {
    // If the area within the lights radius has been modified, queue all the relevant cloud points
    _dirtyPointCloudIndices.clear();
    _dirtyPointCloudIndices.reserve(Scene::_cloudPoints.size());
    for (int j = 0; j < Scene::_cloudPoints.size(); j++) {
        CloudPoint& cloudPoint = Scene::_cloudPoints[j];
        for (auto& light : Scene::_lights) {
            const float lightRadiusSquared = light.radius * light.radius;
            if (light.isDirty) {
                if (Util::DistanceSquared(cloudPoint.position, light.position) < lightRadiusSquared) {
                    _dirtyPointCloudIndices.push_back(j);
                    break;
                }
            }
        }
    }
}

void CalculateDirtyProbeCoords() {

    if (_dirtyProbeCoords.size() == 0) {
        _dirtyProbeCoords.resize(_gridTextureSize);
    }

    //Timer t("_dirtyProbeCoords");
    _dirtyProbeCount = 0;

    // Early out AABB around entire dirty point cloud
    float cloudMinX = 9999;
    float cloudMinY = 9999;
    float cloudMinZ = 9999;
    float cloudMaxX = -9999;
    float cloudMaxY = -9999;
    float cloudMaxZ = -9999;
    for (int& index : _dirtyPointCloudIndices) {
        cloudMinX = std::min(cloudMinX, Scene::_cloudPoints[index].position.x);
        cloudMaxX = std::max(cloudMaxX, Scene::_cloudPoints[index].position.x);
        cloudMinY = std::min(cloudMinY, Scene::_cloudPoints[index].position.y);
        cloudMaxY = std::max(cloudMaxY, Scene::_cloudPoints[index].position.y);
        cloudMinZ = std::min(cloudMinZ, Scene::_cloudPoints[index].position.z);
        cloudMaxZ = std::max(cloudMaxZ, Scene::_cloudPoints[index].position.z);
    }

    for (int i = 0; i < _probeCoordsWithinMapBounds.size(); i++) {

        const glm::vec3 probePosition = _probeWorldPositionsWithinMapBounds[i];

        for (int& index : _dirtyPointCloudIndices) {
            const glm::vec3& cloudPointPosition = Scene::_cloudPoints[index].position;

            // AABB early out
            if (probePosition.x - _maxPropogationDistance > cloudPointPosition.x ||
                probePosition.y - _maxPropogationDistance > cloudPointPosition.y ||
                probePosition.z - _maxPropogationDistance > cloudPointPosition.z ||
                probePosition.x + _maxPropogationDistance < cloudPointPosition.x ||
                probePosition.y + _maxPropogationDistance < cloudPointPosition.y ||
                probePosition.z + _maxPropogationDistance < cloudPointPosition.z) {
                continue;
            }

            if (Util::DistanceSquared(cloudPointPosition, probePosition) < _maxDistanceSquared) {

                // skip probe if cloud point faces away from probe 
                glm::vec3 cloudPointNormal = Scene::_cloudPoints[index].normal;
                if (dot(cloudPointPosition - probePosition, cloudPointNormal) > 0.0) {
                    continue;
                }
                _dirtyProbeCoords[_dirtyProbeCount] = _probeCoordsWithinMapBounds[i];;
                _dirtyProbeCount++;
                break;
            }
        }
    }
}
